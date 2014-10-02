/*
 * Lockless get_user_pages_fast for x86
 *
 * Copyright (C) 2008 Nick Piggin
 * Copyright (C) 2008 Novell Inc.
 */
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/highmem.h>
#include <linux/swap.h>

#include <asm/pgtable.h>

/*
 * Keep irq disabled for no more than BATCH_PAGES pages.
 * Matches PTRS_PER_PTE (or half in non-PAE kernels).
 */
#define BATCH_PAGES	512

static inline pte_t gup_get_pte(pte_t *ptep)
{
#ifndef CONFIG_X86_PAE
	return READ_ONCE(*ptep);
#else
	/*
	 * With get_user_pages_fast, we walk down the pagetables without taking
	 * any locks.  For this we would like to load the pointers atomically,
	 * but that is not possible (without expensive cmpxchg8b) on PAE.  What
	 * we do have is the guarantee that a pte will only either go from not
	 * present to present, or present to not present or both -- it will not
	 * switch to a completely different present page without a TLB flush in
	 * between; something that we are blocking by holding interrupts off.
	 *
	 * Setting ptes from not present to present goes:
	 * ptep->pte_high = h;
	 * smp_wmb();
	 * ptep->pte_low = l;
	 *
	 * And present to not present goes:
	 * ptep->pte_low = 0;
	 * smp_wmb();
	 * ptep->pte_high = 0;
	 *
	 * We must ensure here that the load of pte_low sees l iff pte_high
	 * sees h. We load pte_high *after* loading pte_low, which ensures we
	 * don't see an older value of pte_high.  *Then* we recheck pte_low,
	 * which ensures that we haven't picked up a changed pte high. We might
	 * have got rubbish values from pte_low and pte_high, but we are
	 * guaranteed that pte_low will not have the present bit set *unless*
	 * it is 'l'. And get_user_pages_fast only operates on present ptes, so
	 * we're safe.
	 *
	 * gup_get_pte should not be used or copied outside gup.c without being
	 * very careful -- it does not atomically load the pte or anything that
	 * is likely to be useful for you.
	 */
	pte_t pte;

retry:
	pte.pte_low = ptep->pte_low;
	smp_rmb();
	pte.pte_high = ptep->pte_high;
	smp_rmb();
	if (unlikely(pte.pte_low != ptep->pte_low))
		goto retry;

	return pte;
#endif
}

/*
 * The performance critical leaf functions are made noinline otherwise gcc
 * inlines everything into a single function which results in too much
 * register pressure.
 */
static noinline int gup_pte_range(pmd_t pmd, unsigned long addr,
		unsigned long end, int write, struct page **pages, int *nr)
{
	unsigned long mask;
	pte_t *ptep;

	mask = _PAGE_PRESENT|_PAGE_USER;
	if (write)
		mask |= _PAGE_RW;

	ptep = pte_offset_map(&pmd, addr);
	do {
		pte_t pte = gup_get_pte(ptep);
		struct page *page;

		/* Similar to the PMD case, NUMA hinting must take slow path */
		if (pte_protnone(pte)) {
			pte_unmap(ptep);
			return 0;
		}

		if ((pte_flags(pte) & (mask | _PAGE_SPECIAL)) != mask) {
			pte_unmap(ptep);
			return 0;
		}
		VM_BUG_ON(!pfn_valid(pte_pfn(pte)));
		page = pte_page(pte);
		get_page(page);
		SetPageReferenced(page);
		pages[*nr] = page;
		(*nr)++;

	} while (ptep++, addr += PAGE_SIZE, addr != end);
	pte_unmap(ptep - 1);

	return 1;
}

static inline void get_head_page_multiple(struct page *page, int nr)
{
	VM_BUG_ON_PAGE(page != compound_head(page), page);
	VM_BUG_ON_PAGE(page_count(page) == 0, page);
	atomic_add(nr, &page->_count);
	SetPageReferenced(page);
}

static noinline int gup_huge_pmd(pmd_t pmd, unsigned long addr,
		unsigned long end, int write, struct page **pages, int *nr)
{
	unsigned long mask;
	pte_t pte = *(pte_t *)&pmd;
	struct page *head, *page;
	int refs;

	mask = _PAGE_PRESENT|_PAGE_USER;
	if (write)
		mask |= _PAGE_RW;
	if ((pte_flags(pte) & mask) != mask)
		return 0;
	/* hugepages are never "special" */
	VM_BUG_ON(pte_flags(pte) & _PAGE_SPECIAL);
	VM_BUG_ON(!pfn_valid(pte_pfn(pte)));

	refs = 0;
	head = pte_page(pte);
	page = head + ((addr & ~PMD_MASK) >> PAGE_SHIFT);
	do {
		VM_BUG_ON_PAGE(compound_head(page) != head, page);
		pages[*nr] = page;
		if (PageTail(page))
			get_huge_page_tail(page);
		(*nr)++;
		page++;
		refs++;
	} while (addr += PAGE_SIZE, addr != end);
	get_head_page_multiple(head, refs);

	return 1;
}

static int gup_pmd_range(pud_t pud, unsigned long addr, unsigned long end,
		int write, struct page **pages, int *nr)
{
	unsigned long next;
	pmd_t *pmdp;

	pmdp = pmd_offset(&pud, addr);
	do {
		pmd_t pmd = *pmdp;

		next = pmd_addr_end(addr, end);
		/*
		 * The pmd_trans_splitting() check below explains why
		 * pmdp_splitting_flush has to flush the tlb, to stop
		 * this gup-fast code from running while we set the
		 * splitting bit in the pmd. Returning zero will take
		 * the slow path that will call wait_split_huge_page()
		 * if the pmd is still in splitting state. gup-fast
		 * can't because it has irq disabled and
		 * wait_split_huge_page() would never return as the
		 * tlb flush IPI wouldn't run.
		 */
		if (pmd_none(pmd) || pmd_trans_splitting(pmd))
			return 0;
		if (unlikely(pmd_large(pmd) || !pmd_present(pmd))) {
			/*
			 * NUMA hinting faults need to be handled in the GUP
			 * slowpath for accounting purposes and so that they
			 * can be serialised against THP migration.
			 */
			if (pmd_protnone(pmd))
				return 0;
			if (!gup_huge_pmd(pmd, addr, next, write, pages, nr))
				return 0;
		} else {
			if (!gup_pte_range(pmd, addr, next, write, pages, nr))
				return 0;
		}
	} while (pmdp++, addr = next, addr != end);

	return 1;
}

static noinline int gup_huge_pud(pud_t pud, unsigned long addr,
		unsigned long end, int write, struct page **pages, int *nr)
{
	unsigned long mask;
	pte_t pte = *(pte_t *)&pud;
	struct page *head, *page;
	int refs;

	mask = _PAGE_PRESENT|_PAGE_USER;
	if (write)
		mask |= _PAGE_RW;
	if ((pte_flags(pte) & mask) != mask)
		return 0;
	/* hugepages are never "special" */
	VM_BUG_ON(pte_flags(pte) & _PAGE_SPECIAL);
	VM_BUG_ON(!pfn_valid(pte_pfn(pte)));

	refs = 0;
	head = pte_page(pte);
	page = head + ((addr & ~PUD_MASK) >> PAGE_SHIFT);
	do {
		VM_BUG_ON_PAGE(compound_head(page) != head, page);
		pages[*nr] = page;
		if (PageTail(page))
			get_huge_page_tail(page);
		(*nr)++;
		page++;
		refs++;
	} while (addr += PAGE_SIZE, addr != end);
	get_head_page_multiple(head, refs);

	return 1;
}

static int gup_pud_range(pgd_t pgd, unsigned long addr, unsigned long end,
			int write, struct page **pages, int *nr)
{
	unsigned long next;
	pud_t *pudp;

	pudp = pud_offset(&pgd, addr);
	do {
		pud_t pud = *pudp;

		next = pud_addr_end(addr, end);
		if (pud_none(pud))
			return 0;
		if (unlikely(pud_large(pud))) {
			if (!gup_huge_pud(pud, addr, next, write, pages, nr))
				return 0;
		} else {
			if (!gup_pmd_range(pud, addr, next, write, pages, nr))
				return 0;
		}
	} while (pudp++, addr = next, addr != end);

	return 1;
}

static inline int __get_user_pages_fast_batch(unsigned long start,
					      unsigned long end,
					      int write, struct page **pages)
{
	struct mm_struct *mm = current->mm;
	unsigned long next;
	unsigned long flags;
	pgd_t *pgdp;
	int nr = 0;

	/*
	 * This doesn't prevent pagetable teardown, but does prevent
	 * the pagetables and pages from being freed on x86.
	 *
	 * So long as we atomically load page table pointers versus teardown
	 * (which we do on x86, with the above PAE exception), we can follow the
	 * address down to the the page and take a ref on it.
	 */
	local_irq_save(flags);
	pgdp = pgd_offset(mm, start);
	do {
		pgd_t pgd = *pgdp;

		next = pgd_addr_end(start, end);
		if (pgd_none(pgd))
			break;
		if (!gup_pud_range(pgd, start, next, write, pages, &nr))
			break;
	} while (pgdp++, start = next, start != end);
	local_irq_restore(flags);

	return nr;
}

/*
 * Like get_user_pages_fast() except its IRQ-safe in that it won't fall
 * back to the regular GUP.
 */
int __get_user_pages_fast(unsigned long start, int nr_pages, int write,
			  struct page **pages)
{
	unsigned long len, end, batch_pages;
	int nr, ret;

	start &= PAGE_MASK;
	len = (unsigned long) nr_pages << PAGE_SHIFT;
	end = start + len;
	/*
	 * get_user_pages() handles nr_pages == 0 gracefully, but
	 * gup_fast starts walking the first pagetable in a do {}
	 * while() fashion so it's not robust to handle nr_pages ==
	 * 0. There's no point in being permissive about end < start
	 * either. So this check verifies both nr_pages being non
	 * zero, and that "end" didn't overflow.
	 */
	VM_BUG_ON(end <= start);
	if (unlikely(!access_ok(write ? VERIFY_WRITE : VERIFY_READ,
					(void __user *)start, len)))
		return 0;

	ret = 0;
	for (;;) {
		batch_pages = nr_pages;
		if (batch_pages > BATCH_PAGES && !irqs_disabled())
			batch_pages = BATCH_PAGES;
		len = (unsigned long) batch_pages << PAGE_SHIFT;
		end = start + len;
		nr = __get_user_pages_fast_batch(start, end, write, pages);
		VM_BUG_ON(nr > batch_pages);
		nr_pages -= nr;
		ret += nr;
		if (!nr_pages || nr != batch_pages)
			break;
		start += len;
		pages += batch_pages;
	}

	return ret;
}

static inline int get_user_pages_fast_batch(unsigned long start,
					    unsigned long end,
					    int write, struct page **pages)
{
	struct mm_struct *mm = current->mm;
	unsigned long next;
	pgd_t *pgdp;
	int nr = 0;
	unsigned long orig_start = start;

	/*
	 * This doesn't prevent pagetable teardown, but does prevent
	 * the pagetables and pages from being freed on x86.
	 *
	 * So long as we atomically load page table pointers versus teardown
	 * (which we do on x86, with the above PAE exception), we can follow the
	 * address down to the the page and take a ref on it.
	 */
	local_irq_disable();
	pgdp = pgd_offset(mm, start);
	do {
		pgd_t pgd = *pgdp;

		next = pgd_addr_end(start, end);
		if (pgd_none(pgd)) {
			VM_BUG_ON(nr >= (end-orig_start) >> PAGE_SHIFT);
			break;
		}
		if (!gup_pud_range(pgd, start, next, write, pages, &nr)) {
			VM_BUG_ON(nr >= (end-orig_start) >> PAGE_SHIFT);
			break;
		}
	} while (pgdp++, start = next, start != end);
	local_irq_enable();

	cond_resched();

	return nr;
}

/**
 * get_user_pages_fast() - pin user pages in memory
 * @start:	starting user address
 * @nr_pages:	number of pages from start to pin
 * @write:	whether pages will be written to
 * @pages:	array that receives pointers to the pages pinned.
 * 		Should be at least nr_pages long.
 *
 * Attempt to pin user pages in memory without taking mm->mmap_sem.
 * If not successful, it will fall back to taking the lock and
 * calling get_user_pages().
 *
 * Returns number of pages pinned. This may be fewer than the number
 * requested. If nr_pages is 0 or negative, returns 0. If no pages
 * were pinned, returns -errno.
 */
int get_user_pages_fast(unsigned long start, int nr_pages, int write,
			struct page **pages)
{
	struct mm_struct *mm = current->mm;
	unsigned long len, end, batch_pages;
	int nr, ret;
	unsigned long orig_start;

	start &= PAGE_MASK;
	orig_start = start;
	len = (unsigned long) nr_pages << PAGE_SHIFT;

	end = start + len;
	/*
	 * get_user_pages() handles nr_pages == 0 gracefully, but
	 * gup_fast starts walking the first pagetable in a do {}
	 * while() fashion so it's not robust to handle nr_pages ==
	 * 0. There's no point in being permissive about end < start
	 * either. So this check verifies both nr_pages being non
	 * zero, and that "end" didn't overflow.
	 */
	VM_BUG_ON(end <= start);

	nr = ret = 0;
#ifdef CONFIG_X86_64
	if (end >> __VIRTUAL_MASK_SHIFT)
		goto slow_irqon;
#endif
	for (;;) {
		batch_pages = min(nr_pages, BATCH_PAGES);
		len = (unsigned long) batch_pages << PAGE_SHIFT;
		end = start + len;
		nr = get_user_pages_fast_batch(start, end, write, pages);
		VM_BUG_ON(nr > batch_pages);
		nr_pages -= nr;
		ret += nr;
		if (!nr_pages)
			break;
		if (nr < batch_pages)
			goto slow_irqon;
		start += len;
		pages += batch_pages;
	}

	VM_BUG_ON(ret != (end - orig_start) >> PAGE_SHIFT);
	return ret;

slow_irqon:
	/* Try to get the remaining pages with get_user_pages */
	start += nr << PAGE_SHIFT;
	pages += nr;

	/*
	 * "nr" was the get_user_pages_fast_batch last retval, "ret"
	 * was the sum of all get_user_pages_fast_batch retvals, now
	 * "nr" becomes the sum of all get_user_pages_fast_batch
	 * retvals and "ret" will become the get_user_pages_unlocked
	 * retval.
	 */
	nr = ret;

	ret = get_user_pages_unlocked(current, mm, start,
				      (end - start) >> PAGE_SHIFT,
				      write, 0, pages);

	/* Have to be a bit careful with return values */
	if (nr > 0) {
		if (ret < 0)
			ret = nr;
		else
			ret += nr;
	}

	return ret;
}
