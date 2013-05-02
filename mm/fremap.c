/*
 *   linux/mm/fremap.c
 * 
 * Explicit pagetable population and nonlinear (random) mappings support.
 *
 * started by Ingo Molnar, Copyright (C) 2002, 2003
 */
#include <linux/export.h>
#include <linux/backing-dev.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/file.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/syscalls.h>
#include <linux/mmu_notifier.h>

#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include "internal.h"

static int mm_counter(struct page *page)
{
	return PageAnon(page) ? MM_ANONPAGES : MM_FILEPAGES;
}

static void zap_pte(struct mm_struct *mm, struct vm_area_struct *vma,
			unsigned long addr, pte_t *ptep)
{
	pte_t pte = *ptep;
	struct page *page;
	swp_entry_t entry;

	if (pte_present(pte)) {
		flush_cache_page(vma, addr, pte_pfn(pte));
		pte = ptep_clear_flush(vma, addr, ptep);
		page = vm_normal_page(vma, addr, pte);
		if (page) {
			if (pte_dirty(pte))
				set_page_dirty(page);
			update_hiwater_rss(mm);
			dec_mm_counter(mm, mm_counter(page));
			page_remove_rmap(page);
			page_cache_release(page);
		}
	} else {	/* zap_pte() is not called when pte_none() */
		if (!pte_file(pte)) {
			update_hiwater_rss(mm);
			entry = pte_to_swp_entry(pte);
			if (non_swap_entry(entry)) {
				if (is_migration_entry(entry)) {
					page = migration_entry_to_page(entry);
					dec_mm_counter(mm, mm_counter(page));
				}
			} else {
				free_swap_and_cache(entry);
				dec_mm_counter(mm, MM_SWAPENTS);
			}
		}
		pte_clear_not_present_full(mm, addr, ptep, 0);
	}
}

/*
 * Install a file pte to a given virtual memory address, release any
 * previously existing mapping.
 */
static int install_file_pte(struct mm_struct *mm, struct vm_area_struct *vma,
		unsigned long addr, unsigned long pgoff, pgprot_t prot)
{
	int err = -ENOMEM;
	pte_t *pte, ptfile;
	spinlock_t *ptl;

	pte = get_locked_pte(mm, addr, &ptl);
	if (!pte)
		goto out;

	ptfile = pgoff_to_pte(pgoff);

	if (!pte_none(*pte))
		zap_pte(mm, vma, addr, pte);

	set_pte_at(mm, addr, pte, pte_file_mksoft_dirty(ptfile));
	/*
	 * We don't need to run update_mmu_cache() here because the "file pte"
	 * being installed by install_file_pte() is not a real pte - it's a
	 * non-present entry (like a swap entry), noting what file offset should
	 * be mapped there when there's a fault (in a non-linear vma where
	 * that's not obvious).
	 */
	pte_unmap_unlock(pte, ptl);
	err = 0;
out:
	return err;
}

int generic_file_remap_pages(struct vm_area_struct *vma, unsigned long addr,
			     unsigned long size, pgoff_t pgoff)
{
	struct mm_struct *mm = vma->vm_mm;
	int err;

	do {
		err = install_file_pte(mm, vma, addr, pgoff, vma->vm_page_prot);
		if (err)
			return err;

		size -= PAGE_SIZE;
		addr += PAGE_SIZE;
		pgoff++;
	} while (size);

	return 0;
}
EXPORT_SYMBOL(generic_file_remap_pages);

/**
 * sys_remap_file_pages - remap arbitrary pages of an existing VM_SHARED vma
 * @start: start of the remapped virtual memory range
 * @size: size of the remapped virtual memory range
 * @prot: new protection bits of the range (see NOTE)
 * @pgoff: to-be-mapped page of the backing store file
 * @flags: 0 or MAP_NONBLOCKED - the later will cause no IO.
 *
 * sys_remap_file_pages remaps arbitrary pages of an existing VM_SHARED vma
 * (shared backing store file).
 *
 * This syscall works purely via pagetables, so it's the most efficient
 * way to map the same (large) file into a given virtual window. Unlike
 * mmap()/mremap() it does not create any new vmas. The new mappings are
 * also safe across swapout.
 *
 * NOTE: the @prot parameter right now is ignored (but must be zero),
 * and the vma's default protection is used. Arbitrary protections
 * might be implemented in the future.
 */
SYSCALL_DEFINE5(remap_file_pages, unsigned long, start, unsigned long, size,
		unsigned long, prot, unsigned long, pgoff, unsigned long, flags)
{
	struct mm_struct *mm = current->mm;
	struct address_space *mapping;
	struct vm_area_struct *vma;
	int err = -EINVAL;
	int has_write_lock = 0;
	vm_flags_t vm_flags = 0;

	pr_warn_once("%s (%d) uses deprecated remap_file_pages() syscall. "
			"See Documentation/vm/remap_file_pages.txt.\n",
			current->comm, current->pid);

	if (prot)
		return err;
	/*
	 * Sanitize the syscall parameters:
	 */
	start = start & PAGE_MASK;
	size = size & PAGE_MASK;

	/* Does the address range wrap, or is the span zero-sized? */
	if (start + size <= start)
		return err;

	/* Does pgoff wrap? */
	if (pgoff + (size >> PAGE_SHIFT) < pgoff)
		return err;

	/* Can we represent this offset inside this architecture's pte's? */
#if PTE_FILE_MAX_BITS < BITS_PER_LONG
	if (pgoff + (size >> PAGE_SHIFT) >= (1UL << PTE_FILE_MAX_BITS))
		return err;
#endif

	/* We need down_write() to change vma->vm_flags. */
	down_read(&mm->mmap_sem);
 retry:
	vma = find_vma(mm, start);

	/*
	 * Make sure the vma is shared, that it supports prefaulting,
	 * and that the remapped range is valid and fully within
	 * the single existing vma.
	 */
	if (!vma || !(vma->vm_flags & VM_SHARED))
		goto out;

	if (!vma->vm_ops || !vma->vm_ops->remap_pages)
		goto out;

	if (start < vma->vm_start || start + size > vma->vm_end)
		goto out;

	/* Must set VM_NONLINEAR before any pages are populated. */
	if (!(vma->vm_flags & VM_NONLINEAR)) {
		/*
		 * vm_private_data is used as a swapout cursor
		 * in a VM_NONLINEAR vma.
		 */
		if (vma->vm_private_data)
			goto out;

		/* Don't need a nonlinear mapping, exit success */
		if (pgoff == linear_page_index(vma, start)) {
			err = 0;
			goto out;
		}

		if (!has_write_lock) {
get_write_lock:
			up_read(&mm->mmap_sem);
			down_write(&mm->mmap_sem);
			has_write_lock = 1;
			goto retry;
		}
		mapping = vma->vm_file->f_mapping;
		/*
		 * page_mkclean doesn't work on nonlinear vmas, so if
		 * dirty pages need to be accounted, emulate with linear
		 * vmas.
		 */
		if (mapping_cap_account_dirty(mapping)) {
			unsigned long addr;
			struct file *file = get_file(vma->vm_file);
			/* mmap_region may free vma; grab the info now */
			vm_flags = vma->vm_flags;

			addr = mmap_region(file, start, size, vm_flags, pgoff);
			fput(file);
			if (IS_ERR_VALUE(addr)) {
				err = addr;
			} else {
				BUG_ON(addr != start);
				err = 0;
			}
			goto out_freed;
		}
		mutex_lock(&mapping->i_mmap_mutex);
		flush_dcache_mmap_lock(mapping);
		vma->vm_flags |= VM_NONLINEAR;
		vma_interval_tree_remove(vma, &mapping->i_mmap);
		vma_nonlinear_insert(vma, &mapping->i_mmap_nonlinear);
		flush_dcache_mmap_unlock(mapping);
		mutex_unlock(&mapping->i_mmap_mutex);
	}

	if (vma->vm_flags & VM_LOCKED) {
		/*
		 * drop PG_Mlocked flag for over-mapped range
		 */
		if (!has_write_lock)
			goto get_write_lock;
		vm_flags = vma->vm_flags;
		munlock_vma_pages_range(vma, start, start + size);
		vma->vm_flags = vm_flags;
	}

	mmu_notifier_invalidate_range_start(mm, start, start + size);
	err = vma->vm_ops->remap_pages(vma, start, size, pgoff);
	mmu_notifier_invalidate_range_end(mm, start, start + size);

	/*
	 * We can't clear VM_NONLINEAR because we'd have to do
	 * it after ->populate completes, and that would prevent
	 * downgrading the lock.  (Locks can't be upgraded).
	 */

out:
	if (vma)
		vm_flags = vma->vm_flags;
out_freed:
	if (likely(!has_write_lock))
		up_read(&mm->mmap_sem);
	else
		up_write(&mm->mmap_sem);
	if (!err && ((vm_flags & VM_LOCKED) || !(flags & MAP_NONBLOCK)))
		mm_populate(start, size);

	return err;
}

void double_pt_lock(spinlock_t *ptl1,
		    spinlock_t *ptl2)
	__acquires(ptl1)
	__acquires(ptl2)
{
	spinlock_t *ptl_tmp;

	if (ptl1 > ptl2) {
		/* exchange ptl1 and ptl2 */
		ptl_tmp = ptl1;
		ptl1 = ptl2;
		ptl2 = ptl_tmp;
	}
	/* lock in virtual address order to avoid lock inversion */
	spin_lock(ptl1);
	if (ptl1 != ptl2)
		spin_lock_nested(ptl2, SINGLE_DEPTH_NESTING);
}

void double_pt_unlock(spinlock_t *ptl1,
		      spinlock_t *ptl2)
	__releases(ptl1)
	__releases(ptl2)
{
	spin_unlock(ptl1);
	if (ptl1 != ptl2)
		spin_unlock(ptl2);
}

#define RAP_ALLOW_SRC_HOLES (1UL<<0)

/*
 * The mmap_sem for reading is held by the caller. Just move the page
 * from src_pmd to dst_pmd if possible, and return true if succeeded
 * in moving the page.
 */
static int remap_anon_pages_pte(struct mm_struct *mm,
				pte_t *dst_pte, pte_t *src_pte, pmd_t *src_pmd,
				struct vm_area_struct *dst_vma,
				struct vm_area_struct *src_vma,
				unsigned long dst_addr,
				unsigned long src_addr,
				spinlock_t *dst_ptl,
				spinlock_t *src_ptl,
				unsigned long flags)
{
	struct page *src_page;
	swp_entry_t entry;
	pte_t orig_src_pte, orig_dst_pte;
	struct anon_vma *src_anon_vma, *dst_anon_vma;

	spin_lock(dst_ptl);
	orig_dst_pte = *dst_pte;
	spin_unlock(dst_ptl);
	if (!pte_none(orig_dst_pte))
		return -EEXIST;

	spin_lock(src_ptl);
	orig_src_pte = *src_pte;
	spin_unlock(src_ptl);
	if (pte_none(orig_src_pte)) {
		if (!(flags & RAP_ALLOW_SRC_HOLES))
			return -ENOENT;
		else
			/* nothing to do to remap an hole */
			return 0;
	}

	if (pte_present(orig_src_pte)) {
		/*
		 * Pin the page while holding the lock to be sure the
		 * page isn't freed under us
		 */
		spin_lock(src_ptl);
		if (!pte_same(orig_src_pte, *src_pte)) {
			spin_unlock(src_ptl);
			return -EAGAIN;
		}
		src_page = vm_normal_page(src_vma, src_addr, orig_src_pte);
		if (!src_page || !PageAnon(src_page) ||
		    page_mapcount(src_page) != 1) {
			spin_unlock(src_ptl);
			return -EBUSY;
		}

		get_page(src_page);
		spin_unlock(src_ptl);

		/* block all concurrent rmap walks */
		lock_page(src_page);

		/*
		 * page_referenced_anon walks the anon_vma chain
		 * without the page lock. Serialize against it with
		 * the anon_vma lock, the page lock is not enough.
		 */
		src_anon_vma = page_get_anon_vma(src_page);
		if (!src_anon_vma) {
			/* page was unmapped from under us */
			unlock_page(src_page);
			put_page(src_page);
			return -EAGAIN;
		}
		anon_vma_lock_write(src_anon_vma);

		double_pt_lock(dst_ptl, src_ptl);

		if (!pte_same(*src_pte, orig_src_pte) ||
		    !pte_same(*dst_pte, orig_dst_pte) ||
		    page_mapcount(src_page) != 1) {
			double_pt_unlock(dst_ptl, src_ptl);
			anon_vma_unlock_write(src_anon_vma);
			put_anon_vma(src_anon_vma);
			unlock_page(src_page);
			put_page(src_page);
			return -EAGAIN;
		}

		BUG_ON(!PageAnon(src_page));
		/* the PT lock is enough to keep the page pinned now */
		put_page(src_page);

		dst_anon_vma = (void *) dst_vma->anon_vma + PAGE_MAPPING_ANON;
		ACCESS_ONCE(src_page->mapping) = ((struct address_space *)
						  dst_anon_vma);
		ACCESS_ONCE(src_page->index) = linear_page_index(dst_vma,
								 dst_addr);

		if (!pte_same(ptep_clear_flush(src_vma, src_addr, src_pte),
			      orig_src_pte))
			BUG();

		orig_dst_pte = mk_pte(src_page, dst_vma->vm_page_prot);
		orig_dst_pte = maybe_mkwrite(pte_mkdirty(orig_dst_pte),
					     dst_vma);

		set_pte_at(mm, dst_addr, dst_pte, orig_dst_pte);

		double_pt_unlock(dst_ptl, src_ptl);

		anon_vma_unlock_write(src_anon_vma);
		put_anon_vma(src_anon_vma);

		/* unblock rmap walks */
		unlock_page(src_page);

		mmu_notifier_invalidate_page(mm, src_addr);
	} else {
		if (pte_file(orig_src_pte))
			return -EFAULT;

		entry = pte_to_swp_entry(orig_src_pte);
		if (non_swap_entry(entry)) {
			if (is_migration_entry(entry)) {
				migration_entry_wait(mm, src_pmd, src_addr);
				return -EAGAIN;
			}
			return -EFAULT;
		}

		if (swp_entry_swapcount(entry) != 1)
			return -EBUSY;

		double_pt_lock(dst_ptl, src_ptl);

		if (!pte_same(*src_pte, orig_src_pte) ||
		    !pte_same(*dst_pte, orig_dst_pte) ||
		    swp_entry_swapcount(entry) != 1) {
			double_pt_unlock(dst_ptl, src_ptl);
			return -EAGAIN;
		}

		if (pte_val(ptep_get_and_clear(mm, src_addr, src_pte)) !=
		    pte_val(orig_src_pte))
			BUG();
		set_pte_at(mm, dst_addr, dst_pte, orig_src_pte);

		double_pt_unlock(dst_ptl, src_ptl);
	}

	return 0;
}

static pmd_t *mm_alloc_pmd(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd = NULL;

	pgd = pgd_offset(mm, address);
	pud = pud_alloc(mm, pgd, address);
	if (pud)
		/*
		 * Note that we didn't run this because the pmd was
		 * missing, the *pmd may be already established and in
		 * turn it may also be a trans_huge_pmd.
		 */
		pmd = pmd_alloc(mm, pud, address);
	return pmd;
}

/**
 * sys_remap_anon_pages - remap arbitrary anonymous pages of an existing vma
 * @dst_start: start of the destination virtual memory range
 * @src_start: start of the source virtual memory range
 * @len: length of the virtual memory range
 *
 * sys_remap_anon_pages remaps arbitrary anonymous pages atomically in
 * zero copy. It only works on non shared anonymous pages because
 * those can be relocated without generating non linear anon_vmas in
 * the rmap code.
 *
 * It is the ideal mechanism to handle userspace page faults. Normally
 * the destination vma will have VM_USERFAULT set with
 * madvise(MADV_USERFAULT) while the source vma will have VM_DONTCOPY
 * set with madvise(MADV_DONTFORK).
 *
 * The thread receiving the page during the userland page fault
 * (MADV_USERFAULT) will receive the faulting page in the source vma
 * through the network, storage or any other I/O device (MADV_DONTFORK
 * in the source vma avoids remap_anon_pages to fail with -EBUSY if
 * the process forks before remap_anon_pages is called), then it will
 * call remap_anon_pages to map the page in the faulting address in
 * the destination vma.
 *
 * This syscall works purely via pagetables, so it's the most
 * efficient way to move physical non shared anonymous pages across
 * different virtual addresses. Unlike mremap()/mmap()/munmap() it
 * does not create any new vmas. The mapping in the destination
 * address is atomic.
 *
 * It only works if the vma protection bits are identical from the
 * source and destination vma.
 *
 * It can remap non shared anonymous pages within the same vma too.
 *
 * If the source virtual memory range has any unmapped holes, or if
 * the destination virtual memory range is not a whole unmapped hole,
 * remap_anon_pages will fail respectively with -ENOENT or
 * -EEXIST. This provides a very strict behavior to avoid any chance
 * of memory corruption going unnoticed if there are userland race
 * conditions. Only one thread should resolve the userland page fault
 * at any given time for any given faulting address. This means that
 * if two threads try to both call remap_anon_pages on the same
 * destination address at the same time, the second thread will get an
 * explicit error from this syscall.
 *
 * The syscall retval will return "len" is succesful. The syscall
 * however can be interrupted by fatal signals or errors. If
 * interrupted it will return the number of bytes successfully
 * remapped before the interruption if any, or the negative error if
 * none. It will never return zero. Either it will return an error or
 * an amount of bytes successfully moved. If the retval reports a
 * "short" remap, the remap_anon_pages syscall should be repeated by
 * userland with src+retval, dst+reval, len-retval if it wants to know
 * about the error that interrupted it.
 *
 * The RAP_ALLOW_SRC_HOLES flag can be specified to prevent -ENOENT
 * errors to materialize if there are holes in the source virtual
 * range that is being remapped. The holes will be accounted as
 * successfully remapped in the retval of the syscall. This is mostly
 * useful to remap hugepage naturally aligned virtual regions without
 * knowing if there are transparent hugepage in the regions or not,
 * but preventing the risk of having to split the hugepmd during the
 * remap.
 *
 * If there's any rmap walk that is taking the anon_vma locks without
 * first obtaining the page lock (for example split_huge_page and
 * page_referenced_anon), they will have to verify if the
 * page->mapping has changed after taking the anon_vma lock. If it
 * changed they should release the lock and retry obtaining a new
 * anon_vma, because it means the anon_vma was changed by
 * remap_anon_pages before the lock could be obtained. This is the
 * only additional complexity added to the rmap code to provide this
 * anonymous page remapping functionality.
 */
SYSCALL_DEFINE4(remap_anon_pages,
		unsigned long, dst_start, unsigned long, src_start,
		unsigned long, len, unsigned long, flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *src_vma, *dst_vma;
	long err = -EINVAL;
	pmd_t *src_pmd, *dst_pmd;
	pte_t *src_pte, *dst_pte;
	spinlock_t *dst_ptl, *src_ptl;
	unsigned long src_addr, dst_addr;
	int thp_aligned = -1;
	long moved = 0;

	/*
	 * Sanitize the syscall parameters:
	 */
	if (src_start & ~PAGE_MASK)
		return err;
	if (dst_start & ~PAGE_MASK)
		return err;
	if (len & ~PAGE_MASK)
		return err;
	if (flags & ~RAP_ALLOW_SRC_HOLES)
		return err;

	/* Does the address range wrap, or is the span zero-sized? */
	if (unlikely(src_start + len <= src_start))
		return err;
	if (unlikely(dst_start + len <= dst_start))
		return err;

	down_read(&mm->mmap_sem);

	/*
	 * Make sure the vma is not shared, that the src and dst remap
	 * ranges are both valid and fully within a single existing
	 * vma.
	 */
	src_vma = find_vma(mm, src_start);
	if (!src_vma || (src_vma->vm_flags & VM_SHARED))
		goto out;
	if (src_start < src_vma->vm_start ||
	    src_start + len > src_vma->vm_end)
		goto out;

	dst_vma = find_vma(mm, dst_start);
	if (!dst_vma || (dst_vma->vm_flags & VM_SHARED))
		goto out;
	if (dst_start < dst_vma->vm_start ||
	    dst_start + len > dst_vma->vm_end)
		goto out;

	if (pgprot_val(src_vma->vm_page_prot) !=
	    pgprot_val(dst_vma->vm_page_prot))
		goto out;

	/* only allow remapping if both are mlocked or both aren't */
	if ((src_vma->vm_flags & VM_LOCKED) ^ (dst_vma->vm_flags & VM_LOCKED))
		goto out;

	/* only allow remapping across anonymous vmas */
	if (src_vma->vm_ops || dst_vma->vm_ops)
		goto out;

	/*
	 * Ensure the dst_vma has a anon_vma or this page
	 * would get a NULL anon_vma when moved in the
	 * dst_vma.
	 */
	err = -ENOMEM;
	if (unlikely(anon_vma_prepare(dst_vma)))
		goto out;

	for (src_addr = src_start, dst_addr = dst_start;
	     src_addr < src_start + len; ) {
		spinlock_t *ptl;
		pmd_t dst_pmdval;
		BUG_ON(dst_addr >= dst_start + len);
		src_pmd = mm_find_pmd(mm, src_addr);
		if (unlikely(!src_pmd)) {
			if (!(flags & RAP_ALLOW_SRC_HOLES)) {
				err = -ENOENT;
				break;
			} else {
				src_pmd = mm_alloc_pmd(mm, src_addr);
				if (unlikely(!src_pmd)) {
					err = -ENOMEM;
					break;
				}
			}
		}
		dst_pmd = mm_alloc_pmd(mm, dst_addr);
		if (unlikely(!dst_pmd)) {
			err = -ENOMEM;
			break;
		}

		dst_pmdval = pmd_read_atomic(dst_pmd);
		/*
		 * If the dst_pmd is mapped as THP don't
		 * override it and just be strict.
		 */
		if (unlikely(pmd_trans_huge(dst_pmdval))) {
			err = -EEXIST;
			break;
		}
		if (pmd_trans_huge_lock(src_pmd, src_vma, &ptl) == 1) {
			/*
			 * Check if we can move the pmd without
			 * splitting it. First check the address
			 * alignment to be the same in src/dst.  These
			 * checks don't actually need the PT lock but
			 * it's good to do it here to optimize this
			 * block away at build time if
			 * CONFIG_TRANSPARENT_HUGEPAGE is not set.
			 */
			if (thp_aligned == -1)
				thp_aligned = ((src_addr & ~HPAGE_PMD_MASK) ==
					       (dst_addr & ~HPAGE_PMD_MASK));
			if (!thp_aligned || (src_addr & ~HPAGE_PMD_MASK) ||
			    !pmd_none(dst_pmdval) ||
			    src_start + len - src_addr < HPAGE_PMD_SIZE) {
				spin_unlock(ptl);
				/* Fall through */
				split_huge_page_pmd(src_vma, src_addr,
						    src_pmd);
			} else {
				BUG_ON(dst_addr & ~HPAGE_PMD_MASK);
				err = remap_anon_pages_huge_pmd(mm,
								dst_pmd,
								src_pmd,
								dst_pmdval,
								dst_vma,
								src_vma,
								dst_addr,
								src_addr);
				cond_resched();

				if (!err) {
					dst_addr += HPAGE_PMD_SIZE;
					src_addr += HPAGE_PMD_SIZE;
					moved += HPAGE_PMD_SIZE;
				}

				if ((!err || err == -EAGAIN) &&
				    fatal_signal_pending(current))
					err = -EINTR;

				if (err && err != -EAGAIN)
					break;

				continue;
			}
		}

		if (pmd_none(*src_pmd)) {
			if (!(flags & RAP_ALLOW_SRC_HOLES)) {
				err = -ENOENT;
				break;
			} else {
				if (unlikely(__pte_alloc(mm, src_vma, src_pmd,
							 src_addr))) {
					err = -ENOMEM;
					break;
				}
			}
		}

		/*
		 * We held the mmap_sem for reading so MADV_DONTNEED
		 * can zap transparent huge pages under us, or the
		 * transparent huge page fault can establish new
		 * transparent huge pages under us.
		 */
		if (unlikely(pmd_trans_unstable(src_pmd))) {
			err = -EFAULT;
			break;
		}

		if (unlikely(pmd_none(dst_pmdval)) &&
		    unlikely(__pte_alloc(mm, dst_vma, dst_pmd,
					 dst_addr))) {
			err = -ENOMEM;
			break;
		}
		/* If an huge pmd materialized from under us fail */
		if (unlikely(pmd_trans_huge(*dst_pmd))) {
			err = -EFAULT;
			break;
		}

		BUG_ON(pmd_none(*dst_pmd));
		BUG_ON(pmd_none(*src_pmd));
		BUG_ON(pmd_trans_huge(*dst_pmd));
		BUG_ON(pmd_trans_huge(*src_pmd));

		dst_pte = pte_offset_map(dst_pmd, dst_addr);
		src_pte = pte_offset_map(src_pmd, src_addr);
		dst_ptl = pte_lockptr(mm, dst_pmd);
		src_ptl = pte_lockptr(mm, src_pmd);

		err = remap_anon_pages_pte(mm,
					   dst_pte, src_pte, src_pmd,
					   dst_vma, src_vma,
					   dst_addr, src_addr,
					   dst_ptl, src_ptl, flags);

		pte_unmap(dst_pte);
		pte_unmap(src_pte);
		cond_resched();

		if (!err) {
			dst_addr += PAGE_SIZE;
			src_addr += PAGE_SIZE;
			moved += PAGE_SIZE;
		}

		if ((!err || err == -EAGAIN) &&
		    fatal_signal_pending(current))
			err = -EINTR;

		if (err && err != -EAGAIN)
			break;
	}

out:
	up_read(&mm->mmap_sem);
	BUG_ON(moved < 0);
	BUG_ON(err > 0);
	BUG_ON(!moved && !err);
	return moved ? moved : err;
}
