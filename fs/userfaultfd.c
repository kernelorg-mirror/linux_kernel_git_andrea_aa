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
};

struct userfaultfd_wait_queue {
	unsigned long address;
	wait_queue_t wq;
	bool pending;
	struct userfaultfd_ctx *ctx;
};

#define USERFAULTFD_PROTOCOL ((__u64) 0xaa)
#define USERFAULTFD_UNKNOWN_PROTOCOL ((__u64) -1ULL)

enum {
	USERFAULTFD_STATE_ASK_PROTOCOL,
	USERFAULTFD_STATE_ACK_PROTOCOL,
	USERFAULTFD_STATE_ACK_UNKNOWN_PROTOCOL,
	USERFAULTFD_STATE_RUNNING,
};

/**
 * struct mm_slot - userlandfd information per mm that is being scanned
 * @link: link to the mm_slots hash list
 * @mm: the mm that this information is valid for
 * @ctx: userfaultfd context for this mm
 */
struct mm_slot {
	struct hlist_node link;
	struct mm_struct *mm;
	struct userfaultfd_ctx ctx;
	struct rcu_head rcu_head;
};

#define MM_USERLANDFD_HASH_BITS 10
static DEFINE_HASHTABLE(mm_userlandfd_hash, MM_USERLANDFD_HASH_BITS);

static DEFINE_MUTEX(mm_userlandfd_mutex);

static struct mm_slot *get_mm_slot(struct mm_struct *mm)
{
	struct mm_slot *slot;

	hash_for_each_possible_rcu(mm_userlandfd_hash, slot, link,
				   (unsigned long)mm)
		if (slot->mm == mm)
			return slot;

	return NULL;
}

static void insert_to_mm_userlandfd_hash(struct mm_struct *mm,
					 struct mm_slot *mm_slot)
{
	mm_slot->mm = mm;
	hash_add_rcu(mm_userlandfd_hash, &mm_slot->link, (unsigned long)mm);
}

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
static int userfaultfd_ctx_get(struct userfaultfd_ctx *ctx)
{
	/*
	 * If it's already released don't get it. This can race
	 * against userfaultfd_release, if the race triggers it'll be
	 * handled safely by the handle_userfault main loop
	 * (userfaultfd_release will take the mmap_sem for writing to
	 * flush out all in-flight userfaults). This check is only an
	 * optimization.
	 */
	if (unlikely(ACCESS_ONCE(ctx->released)))
		return 0;
	return atomic_inc_not_zero(&ctx->refcount);
}

static void userfaultfd_free(struct userfaultfd_ctx *ctx)
{
	struct mm_slot *mm_slot = container_of(ctx, struct mm_slot, ctx);

	mutex_lock(&mm_userlandfd_mutex);
	hash_del_rcu(&mm_slot->link);
	mutex_unlock(&mm_userlandfd_mutex);

	kfree_rcu(mm_slot, rcu_head);
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
	if (atomic_dec_and_test(&ctx->refcount))
		userfaultfd_free(ctx);
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
	struct mm_slot *slot;
	struct userfaultfd_ctx *ctx;
	struct userfaultfd_wait_queue uwq;
	int ret;

	BUG_ON(!rwsem_is_locked(&mm->mmap_sem));

	rcu_read_lock();
	slot = get_mm_slot(mm);
	if (!slot) {
		rcu_read_unlock();
		return VM_FAULT_SIGBUS;
	}
	ctx = &slot->ctx;
	if (!userfaultfd_ctx_get(ctx)) {
		rcu_read_unlock();
		return VM_FAULT_SIGBUS;
	}
	rcu_read_unlock();

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
		set_current_state(TASK_INTERRUPTIBLE);
		if (fatal_signal_pending(current)) {
			/*
			 * If we have to fail because the task is
			 * killed just retry the fault either by
			 * returning to userland or through
			 * VM_FAULT_RETRY if we come from a page fault
			 * and a fatal signal is pending.
			 */
			ret = 0;
			if (flags & FAULT_FLAG_KILLABLE) {
				/*
				 * If FAULT_FLAG_KILLABLE is set we
				 * and there's a fatal signal pending
				 * can return VM_FAULT_RETRY
				 * regardless if
				 * FAULT_FLAG_ALLOW_RETRY is set or
				 * not as long as we release the
				 * mmap_sem. The page fault will
				 * return stright to userland then to
				 * handle the fatal signal.
				 */
				up_read(&mm->mmap_sem);
				ret = VM_FAULT_RETRY;
			}
			break;
		}
		if (!uwq.pending || ACCESS_ONCE(ctx->released)) {
			ret = 0;
			if (flags & FAULT_FLAG_ALLOW_RETRY) {
				ret = VM_FAULT_RETRY;
				if (!(flags & FAULT_FLAG_RETRY_NOWAIT))
					up_read(&mm->mmap_sem);
			}
			break;
		}
		if (((FAULT_FLAG_ALLOW_RETRY|FAULT_FLAG_RETRY_NOWAIT) &
		     flags) ==
		    (FAULT_FLAG_ALLOW_RETRY|FAULT_FLAG_RETRY_NOWAIT)) {
			ret = VM_FAULT_RETRY;
			/*
			 * The mmap_sem must not be released if
			 * FAULT_FLAG_RETRY_NOWAIT is set despite we
			 * return VM_FAULT_RETRY (FOLL_NOWAIT case).
			 */
			break;
		}
		spin_unlock(&ctx->fault_wqh.lock);
		up_read(&mm->mmap_sem);

		wake_up_poll(&ctx->fd_wqh, POLLIN);
		schedule();

		down_read(&mm->mmap_sem);
		spin_lock(&ctx->fault_wqh.lock);
	}
	__remove_wait_queue(&ctx->fault_wqh, &uwq.wq);
	__set_current_state(TASK_RUNNING);
	spin_unlock(&ctx->fault_wqh.lock);

	/*
	 * ctx may go away after this if the userfault pseudo fd is
	 * released by another CPU.
	 */
	userfaultfd_ctx_put(ctx);

	return ret;
}

static int userfaultfd_release(struct inode *inode, struct file *file)
{
	struct userfaultfd_ctx *ctx = file->private_data;
	struct mm_slot *mm_slot = container_of(ctx, struct mm_slot, ctx);
	__u64 range[2] = { 0ULL, -1ULL };

	ACCESS_ONCE(ctx->released) = true;

	/*
	 * Flush page faults out of all CPUs to avoid race conditions
	 * against ctx->released. All page faults must be retried
	 * without returning VM_FAULT_SIGBUS if the get_mm_slot and
	 * userfaultfd_ctx_get both succeeds but ctx->released is set.
	 */
	down_write(&mm_slot->mm->mmap_sem);
	up_write(&mm_slot->mm->mmap_sem);

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
	if (range[0] >= range[1])
		return -ERANGE;

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
	struct mm_slot *mm_slot;

	/* Check the UFFD_* constants for consistency.  */
	BUILD_BUG_ON(UFFD_CLOEXEC != O_CLOEXEC);
	BUILD_BUG_ON(UFFD_NONBLOCK != O_NONBLOCK);

	file = ERR_PTR(-EINVAL);
	if (flags & ~UFFD_SHARED_FCNTL_FLAGS)
		goto out;

	mm_slot = kmalloc(sizeof(*mm_slot), GFP_KERNEL);
	file = ERR_PTR(-ENOMEM);
	if (!mm_slot)
		goto out;

	mutex_lock(&mm_userlandfd_mutex);
	file = ERR_PTR(-EBUSY);
	if (get_mm_slot(current->mm))
		goto out_free_unlock;

	atomic_set(&mm_slot->ctx.refcount, 1);
	init_waitqueue_head(&mm_slot->ctx.fault_wqh);
	init_waitqueue_head(&mm_slot->ctx.fd_wqh);
	mm_slot->ctx.flags = flags;
	mm_slot->ctx.state = USERFAULTFD_STATE_ASK_PROTOCOL;
	mm_slot->ctx.released = false;

	file = anon_inode_getfile("[userfaultfd]", &userfaultfd_fops,
				  &mm_slot->ctx,
				  O_RDWR | (flags & UFFD_SHARED_FCNTL_FLAGS));
	if (IS_ERR(file))
	out_free_unlock:
		kfree(mm_slot);
	else
		insert_to_mm_userlandfd_hash(current->mm,
					     mm_slot);
	mutex_unlock(&mm_userlandfd_mutex);
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
