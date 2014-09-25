/*
 *  fs/userfaultfd.c
 *
 *  Copyright (C) 2007  Davide Libenzi <davidel@xmailserver.org>
 *  Copyright (C) 2008-2009 Red Hat, Inc.
 *  Copyright (C) 2014  Red Hat, Inc.
 *
 *  This work is licensed under the terms of the GNU GPL, version 2. See
 *  the COPYING file in the top-level directory.
 *
 *  Some part derived from fs/eventfd.c (anon inode setup) and
 *  mm/ksm.c (mm hashing).
 */

#include <linux/hashtable.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/file.h>
#include <linux/bug.h>
#include <linux/anon_inodes.h>
#include <linux/syscalls.h>
#include <linux/userfaultfd.h>
#include <linux/mempolicy.h>

struct userfaultfd_ctx {
	/* pseudo fd refcounting */
	atomic_t refcount;
	/* waitqueue head for the userfaultfd page faults */
	wait_queue_head_t fault_wqh;
	/* waitqueue head for the pseudo fd to wakeup poll/read */
	wait_queue_head_t fd_wqh;
	/* userfaultfd syscall flags */
	unsigned int flags;
	/* state machine */
	unsigned int state;
	/* released */
	bool released;
	/* mm with one ore more vmas attached to this userfaultfd_ctx */
	struct mm_struct *mm;
};

struct userfaultfd_wait_queue {
	unsigned long address;
	wait_queue_t wq;
	bool pending;
	struct userfaultfd_ctx *ctx;
};

#define USERFAULTFD_PROTOCOL ((__u64) 0xaa)
#define USERFAULTFD_UNKNOWN_PROTOCOL ((__u64) -1ULL)

#define USERFAULTFD_RANGE_REGISTER ((__u64) 0x1)
#define USERFAULTFD_RANGE_UNREGISTER ((__u64) 0x2)
#define USERFAULTFD_RANGE_MASK (~((__u64) 0x3))

enum {
	USERFAULTFD_STATE_ASK_PROTOCOL,
	USERFAULTFD_STATE_ACK_PROTOCOL,
	USERFAULTFD_STATE_ACK_UNKNOWN_PROTOCOL,
	USERFAULTFD_STATE_RUNNING,
};

static int userfaultfd_wake_function(wait_queue_t *wq, unsigned mode,
				     int wake_flags, void *key)
{
	unsigned long *range = key;
	int ret;
	struct userfaultfd_wait_queue *uwq;

	uwq = container_of(wq, struct userfaultfd_wait_queue, wq);
	ret = 0;
	/* don't wake the pending ones to avoid reads to block */
	if (uwq->pending && !ACCESS_ONCE(uwq->ctx->released))
		goto out;
	if (range[0] > uwq->address || range[1] <= uwq->address)
		goto out;
	ret = wake_up_state(wq->private, mode);
	if (ret)
		/* wake only once, autoremove behavior */
		list_del_init(&wq->task_list);
out:
	return ret;
}

/**
 * userfaultfd_ctx_get - Acquires a reference to the internal userfaultfd
 * context.
 * @ctx: [in] Pointer to the userfaultfd context.
 *
 * Returns: In case of success, returns not zero.
 */
static void userfaultfd_ctx_get(struct userfaultfd_ctx *ctx)
{
	if (!atomic_inc_not_zero(&ctx->refcount))
		BUG();
}

/**
 * userfaultfd_ctx_put - Releases a reference to the internal userfaultfd
 * context.
 * @ctx: [in] Pointer to userfaultfd context.
 *
 * The userfaultfd context reference must have been previously acquired either
 * with userfaultfd_ctx_get() or userfaultfd_ctx_fdget().
 */
static void userfaultfd_ctx_put(struct userfaultfd_ctx *ctx)
{
	if (atomic_dec_and_test(&ctx->refcount)) {
		mmdrop(ctx->mm);
		kfree(ctx);
	}
}

/*
 * The locking rules involved in returning VM_FAULT_RETRY depending on
 * FAULT_FLAG_ALLOW_RETRY, FAULT_FLAG_RETRY_NOWAIT and
 * FAULT_FLAG_KILLABLE are not straightforward. The "Caution"
 * recommendation in __lock_page_or_retry is not an understatement.
 *
 * If FAULT_FLAG_ALLOW_RETRY is set, the mmap_sem must be released
 * before returning VM_FAULT_RETRY only if FAULT_FLAG_RETRY_NOWAIT is
 * not set.
 *
 * If FAULT_FLAG_ALLOW_RETRY is set but FAULT_FLAG_KILLABLE is not
 * set, VM_FAULT_RETRY can still be returned if and only if there are
 * fatal_signal_pending()s, and the mmap_sem must be released before
 * returning it.
 */
int handle_userfault(struct vm_area_struct *vma, unsigned long address,
		     unsigned int flags)
{
	struct mm_struct *mm = vma->vm_mm;
	struct userfaultfd_ctx *ctx;
	struct userfaultfd_wait_queue uwq;

	BUG_ON(!rwsem_is_locked(&mm->mmap_sem));

	ctx = vma->vm_userfaultfd_ctx.ctx;
	if (!ctx)
		return VM_FAULT_SIGBUS;

	BUG_ON(ctx->mm != mm);

	/*
	 * If it's already released don't get it. This avoids to loop
	 * in __get_user_pages if userfaultfd_release waits on the
	 * caller of handle_userfault to release the mmap_sem.
	 */
	if (unlikely(ACCESS_ONCE(ctx->released)))
		return VM_FAULT_SIGBUS;

	/* check that we can return VM_FAULT_RETRY */
	if (unlikely(!(flags & FAULT_FLAG_ALLOW_RETRY))) {
		/*
		 * Validate the invariant that nowait must allow retry
		 * to be sure not to return SIGBUS erroneously on
		 * nowait invocations.
		 */
		BUG_ON(flags & FAULT_FLAG_RETRY_NOWAIT);
#ifdef CONFIG_DEBUG_VM
		if (printk_ratelimit()) {
			printk(KERN_WARNING
			       "FAULT_FLAG_ALLOW_RETRY missing %x\n", flags);
			dump_stack();
		}
#endif
		return VM_FAULT_SIGBUS;
	}

	/*
	 * Handle nowait, not much to do other than tell it to retry
	 * and wait.
	 */
	if (flags & FAULT_FLAG_RETRY_NOWAIT)
		return VM_FAULT_RETRY;

	/* take the reference before dropping the mmap_sem */
	userfaultfd_ctx_get(ctx);

	/* be gentle and immediately relinquish the mmap_sem */
	up_read(&mm->mmap_sem);

	init_waitqueue_func_entry(&uwq.wq, userfaultfd_wake_function);
	uwq.wq.private = current;
	uwq.address = address;
	uwq.pending = true;
	uwq.ctx = ctx;

	spin_lock(&ctx->fault_wqh.lock);
	/*
	 * After the __add_wait_queue the uwq is visible to userland
	 * through poll/read().
	 */
	__add_wait_queue(&ctx->fault_wqh, &uwq.wq);
	for (;;) {
		set_current_state(TASK_KILLABLE);
		if (!uwq.pending || ACCESS_ONCE(ctx->released) ||
		    fatal_signal_pending(current))
			break;
		spin_unlock(&ctx->fault_wqh.lock);

		wake_up_poll(&ctx->fd_wqh, POLLIN);
		schedule();

		spin_lock(&ctx->fault_wqh.lock);
	}
	__remove_wait_queue(&ctx->fault_wqh, &uwq.wq);
	__set_current_state(TASK_RUNNING);
	spin_unlock(&ctx->fault_wqh.lock);

	/*
	 * ctx may go away after this if the userfault pseudo fd is
	 * already released.
	 */
	userfaultfd_ctx_put(ctx);

	return VM_FAULT_RETRY;
}

static int userfaultfd_release(struct inode *inode, struct file *file)
{
	struct userfaultfd_ctx *ctx = file->private_data;
	struct mm_struct *mm = ctx->mm;
	struct vm_area_struct *vma, *prev;
	__u64 range[2] = { 0ULL, -1ULL };

	ACCESS_ONCE(ctx->released) = true;

	/*
	 * Flush page faults out of all CPUs. NOTE: all page faults
	 * must be retried without returning VM_FAULT_SIGBUS if
	 * userfaultfd_ctx_get() succeeds but vma->vma_userfault_ctx
	 * changes while handle_userfault released the mmap_sem. So
	 * it's critical that released is set to true (above), before
	 * taking the mmap_sem for writing.
	 */
	down_write(&mm->mmap_sem);
	prev = NULL;
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma->vm_userfaultfd_ctx.ctx != ctx)
			continue;
		prev = vma_merge(mm, prev, vma->vm_start, vma->vm_end,
				 vma->vm_flags, vma->anon_vma,
				 vma->vm_file, vma->vm_pgoff,
				 vma_policy(vma),
				 NULL_VM_USERFAULTFD_CTX);
		if (prev)
			vma = prev;
		else
			prev = vma;
		vma->vm_userfaultfd_ctx = NULL_VM_USERFAULTFD_CTX;
	}
	up_write(&mm->mmap_sem);

	/*
	 * After no new page faults can wait on this fautl_wqh, flush
	 * the last page faults that may have been already waiting on
	 * the fault_wqh.
	 */
	spin_lock(&ctx->fault_wqh.lock);
	__wake_up_locked_key(&ctx->fault_wqh, TASK_NORMAL, 0, range);
	spin_unlock(&ctx->fault_wqh.lock);

	wake_up_poll(&ctx->fd_wqh, POLLHUP);
	userfaultfd_ctx_put(ctx);
	return 0;
}

static inline unsigned long find_userfault(struct userfaultfd_ctx *ctx,
					   struct userfaultfd_wait_queue **uwq,
					   unsigned int events_filter)
{
	wait_queue_t *wq;
	struct userfaultfd_wait_queue *_uwq;
	unsigned int events = 0;

	BUG_ON(!events_filter);

	spin_lock(&ctx->fault_wqh.lock);
	list_for_each_entry(wq, &ctx->fault_wqh.task_list, task_list) {
		_uwq = container_of(wq, struct userfaultfd_wait_queue, wq);
		if (_uwq->pending) {
			if (!(events & POLLIN) && (events_filter & POLLIN)) {
				events |= POLLIN;
				if (uwq)
					*uwq = _uwq;
			}
		} else if (events_filter & POLLOUT)
			events |= POLLOUT;
		if (events == events_filter)
			break;
	}
	spin_unlock(&ctx->fault_wqh.lock);

	return events;
}

static unsigned int userfaultfd_poll(struct file *file, poll_table *wait)
{
	struct userfaultfd_ctx *ctx = file->private_data;

	poll_wait(file, &ctx->fd_wqh, wait);

	switch (ctx->state) {
	case USERFAULTFD_STATE_ASK_PROTOCOL:
		return POLLOUT;
	case USERFAULTFD_STATE_ACK_PROTOCOL:
		return POLLIN;
	case USERFAULTFD_STATE_ACK_UNKNOWN_PROTOCOL:
		return POLLIN;
	case USERFAULTFD_STATE_RUNNING:
		return find_userfault(ctx, NULL, POLLIN|POLLOUT);
	default:
		BUG();
	}
}

static ssize_t userfaultfd_ctx_read(struct userfaultfd_ctx *ctx, int no_wait,
				    __u64 *addr)
{
	ssize_t ret;
	DECLARE_WAITQUEUE(wait, current);
	struct userfaultfd_wait_queue *uwq = NULL;

	if (ctx->state == USERFAULTFD_STATE_ASK_PROTOCOL) {
		return -EINVAL;
	} else if (ctx->state == USERFAULTFD_STATE_ACK_PROTOCOL) {
		*addr = USERFAULTFD_PROTOCOL;
		ctx->state = USERFAULTFD_STATE_RUNNING;
		return 0;
	} else if (ctx->state == USERFAULTFD_STATE_ACK_UNKNOWN_PROTOCOL) {
		*addr = USERFAULTFD_UNKNOWN_PROTOCOL;
		ctx->state = USERFAULTFD_STATE_ASK_PROTOCOL;
		return 0;
	}
	BUG_ON(ctx->state != USERFAULTFD_STATE_RUNNING);

	spin_lock(&ctx->fd_wqh.lock);
	__add_wait_queue(&ctx->fd_wqh, &wait);
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		/* always take the fd_wqh lock before the fault_wqh lock */
		if (find_userfault(ctx, &uwq, POLLIN)) {
			uwq->pending = false;
			*addr = uwq->address;
			ret = 0;
			break;
		}
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
		if (no_wait) {
			ret = -EAGAIN;
			break;
		}
		spin_unlock(&ctx->fd_wqh.lock);
		schedule();
		spin_lock_irq(&ctx->fd_wqh.lock);
	}
	__remove_wait_queue(&ctx->fd_wqh, &wait);
	__set_current_state(TASK_RUNNING);
	if (ret == 0) {
		if (waitqueue_active(&ctx->fd_wqh))
			wake_up_locked_poll(&ctx->fd_wqh, POLLOUT);
	}
	spin_unlock_irq(&ctx->fd_wqh.lock);

	return ret;
}

static ssize_t userfaultfd_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct userfaultfd_ctx *ctx = file->private_data;
	ssize_t ret;
	/* careful to always initialize addr if ret == 0 */
	__u64 uninitialized_var(addr);

	if (count < sizeof(addr))
		return -EINVAL;
	ret = userfaultfd_ctx_read(ctx, file->f_flags & O_NONBLOCK, &addr);
	if (ret < 0)
		return ret;

	return put_user(addr, (__u64 __user *) buf) ? -EFAULT : sizeof(addr);
}

static int wake_userfault(struct userfaultfd_ctx *ctx, __u64 *range)
{
	wait_queue_t *wq;
	struct userfaultfd_wait_queue *uwq;
	int ret = -ENOENT;

	spin_lock(&ctx->fault_wqh.lock);
	list_for_each_entry(wq, &ctx->fault_wqh.task_list, task_list) {
		uwq = container_of(wq, struct userfaultfd_wait_queue, wq);
		if (uwq->pending)
			continue;
		if (uwq->address >= range[0] &&
		    uwq->address < range[1]) {
			ret = 0;
			/* wake all in the range and autoremove */
			__wake_up_locked_key(&ctx->fault_wqh, TASK_NORMAL, 0,
					     range);
			break;
		}
	}
	spin_unlock(&ctx->fault_wqh.lock);

	return ret;
}

static ssize_t userfaultfd_range_register(struct userfaultfd_ctx *ctx,
					  unsigned long start,
					  unsigned long end)
{
	struct mm_struct *mm = ctx->mm;
	struct vm_area_struct *vma, *prev;
	int ret;

	down_write(&mm->mmap_sem);
	vma = find_vma(mm, start);
	if (!vma)
		return -ENOMEM;
	if (vma->vm_start >= end)
		return -EINVAL;

	prev = vma->vm_prev;
	if (vma->vm_start < start)
		prev = vma;

	ret = 0;
	/* we got an overlap so start the splitting */
	do {
		if (vma->vm_userfaultfd_ctx.ctx == ctx)
			goto next;
		if (vma->vm_userfaultfd_ctx.ctx) {
			ret = -EBUSY;
			break;
		}
		prev = vma_merge(mm, prev, start, end, vma->vm_flags,
				 vma->anon_vma, vma->vm_file, vma->vm_pgoff,
				 vma_policy(vma),
				 ((struct vm_userfaultfd_ctx){ ctx }));
		if (prev) {
			vma = prev;
			vma->vm_userfaultfd_ctx.ctx = ctx;
			goto next;
		}
		if (vma->vm_start < start) {
			ret = split_vma(mm, vma, start, 1);
			if (ret < 0)
				break;
		}
		if (vma->vm_end > end) {
			ret = split_vma(mm, vma, end, 0);
			if (ret < 0)
				break;
		}
		vma->vm_userfaultfd_ctx.ctx = ctx;
	next:
		start = vma->vm_end;
		vma = vma->vm_next;
	} while (vma && vma->vm_start < end);
	up_write(&mm->mmap_sem);

	return ret;
}

static ssize_t userfaultfd_range_unregister(struct userfaultfd_ctx *ctx,
					    unsigned long start,
					    unsigned long end)
{
	struct mm_struct *mm = ctx->mm;
	struct vm_area_struct *vma, *prev;
	int ret;

	down_write(&mm->mmap_sem);
	vma = find_vma(mm, start);
	if (!vma)
		return -ENOMEM;
	if (vma->vm_start >= end)
		return -EINVAL;

	prev = vma->vm_prev;
	if (vma->vm_start < start)
		prev = vma;

	ret = 0;
	/* we got an overlap so start the splitting */
	do {
		if (!vma->vm_userfaultfd_ctx.ctx)
			goto next;
		if (vma->vm_userfaultfd_ctx.ctx != ctx) {
			ret = -EBUSY;
			break;
		}
		prev = vma_merge(mm, prev, start, end, vma->vm_flags,
				 vma->anon_vma, vma->vm_file, vma->vm_pgoff,
				 vma_policy(vma),
				 NULL_VM_USERFAULTFD_CTX);
		if (prev) {
			vma = prev;
			vma->vm_userfaultfd_ctx = NULL_VM_USERFAULTFD_CTX;
			goto next;
		}
		if (vma->vm_start < start) {
			ret = split_vma(mm, vma, start, 1);
			if (ret < 0)
				break;
		}
		if (vma->vm_end > end) {
			ret = split_vma(mm, vma, end, 0);
			if (ret < 0)
				break;
		}
		vma->vm_userfaultfd_ctx.ctx = NULL;
	next:
		start = vma->vm_end;
		vma = vma->vm_next;
	} while (vma && vma->vm_start < end);
	up_write(&mm->mmap_sem);

	return ret;
}

static ssize_t userfaultfd_handle_range(struct userfaultfd_ctx *ctx,
					__u64 *range)
{
	unsigned long start, end;

	start = range[0] & USERFAULTFD_RANGE_MASK;
	end = range[1];
	BUG_ON(end <= start);
	if (end > TASK_SIZE)
		return -ENOMEM;

	if (range[0] & USERFAULTFD_RANGE_REGISTER) {
		BUG_ON(range[0] & USERFAULTFD_RANGE_UNREGISTER);
		return userfaultfd_range_register(ctx, start, end);
	} else {
		BUG_ON(!(range[0] & USERFAULTFD_RANGE_UNREGISTER));
		return userfaultfd_range_unregister(ctx, start, end);
	}
}

static ssize_t userfaultfd_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct userfaultfd_ctx *ctx = file->private_data;
	__u64 range[2];

	if (ctx->state == USERFAULTFD_STATE_ASK_PROTOCOL) {
		__u64 protocol;
		if (count < sizeof(__u64))
			return -EINVAL;
		if (copy_from_user(&protocol, buf, sizeof(protocol)))
			return -EFAULT;
		if (protocol != USERFAULTFD_PROTOCOL) {
			/* we'll offer the supported protocol in the ack */
			printk_once(KERN_INFO
				    "userfaultfd protocol not available\n");
			ctx->state = USERFAULTFD_STATE_ACK_UNKNOWN_PROTOCOL;
		} else
			ctx->state = USERFAULTFD_STATE_ACK_PROTOCOL;
		return sizeof(protocol);
	} else if (ctx->state == USERFAULTFD_STATE_ACK_PROTOCOL)
		return -EINVAL;

	BUG_ON(ctx->state != USERFAULTFD_STATE_RUNNING);

	if (count < sizeof(range))
		return -EINVAL;
	if (copy_from_user(&range, buf, sizeof(range)))
		return -EFAULT;
	/* the range mask requires 2 bits */
	BUILD_BUG_ON(PAGE_SHIFT < 2);
	if (range[0] & ~PAGE_MASK & USERFAULTFD_RANGE_MASK)
		return -EINVAL;
	if ((range[0] & ~USERFAULTFD_RANGE_MASK) == ~USERFAULTFD_RANGE_MASK)
		return -EINVAL;
	if (range[1] & ~PAGE_MASK)
		return -EINVAL;
	if ((range[0] & PAGE_MASK) >= (range[1] & PAGE_MASK))
		return -ERANGE;

	/* handle the register/unregister commands */
	if (range[0] & ~USERFAULTFD_RANGE_MASK) {
		ssize_t ret = userfaultfd_handle_range(ctx, range);
		BUG_ON(ret > 0);
		return ret < 0 ? ret : sizeof(range);
	}

	/* always take the fd_wqh lock before the fault_wqh lock */
	if (find_userfault(ctx, NULL, POLLOUT))
		if (!wake_userfault(ctx, range))
			return sizeof(range);

	return -ENOENT;
}

#ifdef CONFIG_PROC_FS
static void userfaultfd_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct userfaultfd_ctx *ctx = f->private_data;
	wait_queue_t *wq;
	struct userfaultfd_wait_queue *uwq;
	unsigned long pending = 0, total = 0;

	spin_lock(&ctx->fault_wqh.lock);
	list_for_each_entry(wq, &ctx->fault_wqh.task_list, task_list) {
		uwq = container_of(wq, struct userfaultfd_wait_queue, wq);
		if (uwq->pending)
			pending++;
		total++;
	}
	spin_unlock(&ctx->fault_wqh.lock);

	/*
	 * If more protocols will be added, there will be all shown
	 * separated by a space. Like this:
	 *	protocols: 0xaa 0xbb
	 */
	seq_printf(m, "pending:\t%lu\ntotal:\t%lu\nprotocols:\t%Lx\n",
		   pending, total, USERFAULTFD_PROTOCOL);
}
#endif

static const struct file_operations userfaultfd_fops = {
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= userfaultfd_show_fdinfo,
#endif
	.release	= userfaultfd_release,
	.poll		= userfaultfd_poll,
	.read		= userfaultfd_read,
	.write		= userfaultfd_write,
	.llseek		= noop_llseek,
};

/**
 * userfaultfd_file_create - Creates an userfaultfd file pointer.
 * @flags: Flags for the userfaultfd file.
 *
 * This function creates an userfaultfd file pointer, w/out installing
 * it into the fd table. This is useful when the userfaultfd file is
 * used during the initialization of data structures that require
 * extra setup after the userfaultfd creation. So the userfaultfd
 * creation is split into the file pointer creation phase, and the
 * file descriptor installation phase.  In this way races with
 * userspace closing the newly installed file descriptor can be
 * avoided.  Returns an userfaultfd file pointer, or a proper error
 * pointer.
 */
static struct file *userfaultfd_file_create(int flags)
{
	struct file *file;
	struct userfaultfd_ctx *ctx;

	BUG_ON(!current->mm);

	/* Check the UFFD_* constants for consistency.  */
	BUILD_BUG_ON(UFFD_CLOEXEC != O_CLOEXEC);
	BUILD_BUG_ON(UFFD_NONBLOCK != O_NONBLOCK);

	file = ERR_PTR(-EINVAL);
	if (flags & ~UFFD_SHARED_FCNTL_FLAGS)
		goto out;

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	file = ERR_PTR(-ENOMEM);
	if (!ctx)
		goto out;

	atomic_set(&ctx->refcount, 1);
	init_waitqueue_head(&ctx->fault_wqh);
	init_waitqueue_head(&ctx->fd_wqh);
	ctx->flags = flags;
	ctx->state = USERFAULTFD_STATE_ASK_PROTOCOL;
	ctx->released = false;
	ctx->mm = current->mm;
	/* prevent the mm struct to be freed */
	atomic_inc(&ctx->mm->mm_count);

	file = anon_inode_getfile("[userfaultfd]", &userfaultfd_fops, ctx,
				  O_RDWR | (flags & UFFD_SHARED_FCNTL_FLAGS));
	if (IS_ERR(file))
		kfree(ctx);
out:
	return file;
}

SYSCALL_DEFINE1(userfaultfd, int, flags)
{
	int fd, error;
	struct file *file;

	error = get_unused_fd_flags(flags & UFFD_SHARED_FCNTL_FLAGS);
	if (error < 0)
		return error;
	fd = error;

	file = userfaultfd_file_create(flags);
	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		goto err_put_unused_fd;
	}
	fd_install(fd, file);

	return fd;

err_put_unused_fd:
	put_unused_fd(fd);

	return error;
}
