/*
 *  Copyright (C) 2012  Red Hat, Inc.
 *
 *  This work is licensed under the terms of the GNU GPL, version 2. See
 *  the COPYING file in the top-level directory.
 *
 *  Boot with "numa=fake=2" to test on non NUMA systems.
 */

#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/kthread.h>
#include <linux/mmu_notifier.h>
#include <linux/freezer.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/swap.h>
#include <linux/autonuma.h>
#include <asm/tlbflush.h>
#include <asm/pgtable.h>
#include <trace/events/numa.h>

unsigned long autonuma_flags __read_mostly =
	(1<<AUTONUMA_POSSIBLE_FLAG)
#ifdef CONFIG_AUTONUMA_DEFAULT_ENABLED
	|(1<<AUTONUMA_ENABLED_FLAG)
#endif
	|(0<<AUTONUMA_SCAN_PMD_FLAG)
	|(1<<AUTONUMA_MM_WORKING_SET_FLAG)
	|(1<<AUTONUMA_MIGRATE_ALLOW_FIRST_FAULT_FLAG);

static DEFINE_MUTEX(knumad_mm_mutex);

/* knuma_scand */
static unsigned int scan_sleep_millisecs __read_mostly = 100;
static unsigned int scan_sleep_pass_millisecs __read_mostly = 10000;
static unsigned int pages_to_scan __read_mostly = 128*1024*1024/PAGE_SIZE;
static DECLARE_WAIT_QUEUE_HEAD(knuma_scand_wait);
static unsigned long full_scans;
static unsigned long pages_scanned;

/* page migration rate limiting control */
static unsigned int migrate_sleep_millisecs __read_mostly = 100;
static unsigned int pages_to_migrate __read_mostly = 128*1024*1024/PAGE_SIZE;
static volatile unsigned long pages_migrated;

static struct knuma_scand_data {
	struct list_head mm_head; /* entry: mm->mm_autonuma->mm_node */
	struct mm_struct *mm;
	unsigned long address;
	unsigned long *mm_numa_fault_tmp;
} knuma_scand_data = {
	.mm_head = LIST_HEAD_INIT(knuma_scand_data.mm_head),
};

/* caller already holds the compound_lock */
void autonuma_migrate_split_huge_page(struct page *page,
				      struct page *page_tail)
{
	int last_nid;
	struct page_autonuma *page_autonuma, *page_tail_autonuma;

	if (!autonuma_possible())
		return;

	page_autonuma = lookup_page_autonuma(page);
	page_tail_autonuma = lookup_page_autonuma(page_tail);

	VM_BUG_ON(page_tail_autonuma->autonuma_last_nid != -1);

	last_nid = ACCESS_ONCE(page_autonuma->autonuma_last_nid);
	if (last_nid >= 0)
		page_tail_autonuma->autonuma_last_nid = last_nid;
}

static int sync_isolate_migratepages(struct list_head *migratepages,
				     struct page *page,
				     struct pglist_data *pgdat,
				     bool *migrated)
{
	struct zone *zone;
	struct lruvec *lruvec;
	int nr_subpages;
	struct page *subpage;
	int ret = 0;

	nr_subpages = 1;
	if (PageTransHuge(page)) {
		nr_subpages = HPAGE_PMD_NR;
		VM_BUG_ON(!PageAnon(page));
		/* FIXME: remove split_huge_page */
		if (unlikely(split_huge_page(page))) {
			autonuma_printk("autonuma migrate THP free\n");
			goto out;
		}
	}

	/* All THP subpages are guaranteed to be in the same zone */
	zone = page_zone(page);

	for (subpage = page; subpage < page+nr_subpages; subpage++) {
		spin_lock_irq(&zone->lru_lock);

		/* Must run under the lru_lock and before page isolation */
		lruvec = mem_cgroup_page_lruvec(subpage, zone);

		if (!__isolate_lru_page(subpage, ISOLATE_ASYNC_MIGRATE)) {
			VM_BUG_ON(PageTransCompound(subpage));
			del_page_from_lru_list(subpage, lruvec,
					       page_lru(subpage));
			inc_zone_state(zone, page_is_file_cache(subpage) ?
				       NR_ISOLATED_FILE : NR_ISOLATED_ANON);
			spin_unlock_irq(&zone->lru_lock);

			list_add(&subpage->lru, migratepages);
			ret++;
		} else {
			/* losing page */
			spin_unlock_irq(&zone->lru_lock);
		}
	}

	/*
	 * Pin the head subpage at least until the first
	 * __isolate_lru_page succeeds (__isolate_lru_page pins it
	 * again when it succeeds). If we unpin before
	 * __isolate_lru_page successd, the page could be freed and
	 * reallocated out from under us. Thus our previous checks on
	 * the page, and the split_huge_page, would be worthless.
	 *
	 * We really only need to do this if "ret > 0" but it doesn't
	 * hurt to do it unconditionally as nobody can reference
	 * "page" anymore after this and so we can avoid an "if (ret >
	 * 0)" branch here.
	 */
	put_page(page);
	/*
	 * Tell the caller we already released its pin, to avoid a
	 * double free.
	 */
	*migrated = true;

out:
	return ret;
}

static bool autonuma_balance_pgdat(struct pglist_data *pgdat,
				   int nr_migrate_pages)
{
	/* FIXME: this only check the wmarks, make it move
	 * "unused" memory or pagecache by queuing it to
	 * pgdat->autonuma_migrate_head[pgdat->node_id].
	 */
	int z;
	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
			continue;

		if (zone->all_unreclaimable)
			continue;

		/*
		 * FIXME: in theory we're ok if we can obtain
		 * pages_to_migrate pages from all zones, it doesn't
		 * need to be all in a single zone. We care about the
		 * pgdat, not the zone.
		 */

		/*
		 * Try not to wakeup kswapd by allocating
		 * pages_to_migrate pages.
		 */
		if (!zone_watermark_ok(zone, 0,
				       high_wmark_pages(zone) +
				       nr_migrate_pages,
				       0, 0))
			continue;
		return true;
	}
	return false;
}

static struct page *alloc_migrate_dst_page(struct page *page,
					   unsigned long data,
					   int **result)
{
	int nid = (int) data;
	struct page *newpage;
	struct page_autonuma *page_autonuma, *newpage_autonuma;
	newpage = alloc_pages_exact_node(nid,
					 (GFP_HIGHUSER_MOVABLE | GFP_THISNODE |
					  __GFP_NOMEMALLOC | __GFP_NORETRY |
					  __GFP_NOWARN | __GFP_NO_KSWAPD) &
					 ~GFP_IOFS, 0);
	if (newpage) {
		page_autonuma = lookup_page_autonuma(page);
		newpage_autonuma = lookup_page_autonuma(newpage);
		newpage_autonuma->autonuma_last_nid =
			page_autonuma->autonuma_last_nid;
	}
	return newpage;
}

static inline void autonuma_migrate_lock(int nid)
{
	spin_lock(&NODE_DATA(nid)->autonuma_migrate_lock);
}

static inline void autonuma_migrate_unlock(int nid)
{
	spin_unlock(&NODE_DATA(nid)->autonuma_migrate_lock);
}

static bool autonuma_migrate_page(struct page *page, int dst_nid,
				  int page_nid, bool *migrated)
{
	int isolated = 0;
	LIST_HEAD(migratepages);
	struct pglist_data *pgdat = NODE_DATA(dst_nid);
	int nr_pages = hpage_nr_pages(page);
	unsigned long autonuma_migrate_nr_pages = 0;

	autonuma_migrate_lock(dst_nid);
	if (time_after(jiffies, pgdat->autonuma_migrate_last_jiffies +
		       msecs_to_jiffies(migrate_sleep_millisecs))) {
		autonuma_migrate_nr_pages = pgdat->autonuma_migrate_nr_pages;
		pgdat->autonuma_migrate_nr_pages = 0;
		pgdat->autonuma_migrate_last_jiffies = jiffies;
	}
	if (pgdat->autonuma_migrate_nr_pages >= pages_to_migrate) {
		autonuma_migrate_unlock(dst_nid);
		goto out;
	}
	pgdat->autonuma_migrate_nr_pages += nr_pages;
	autonuma_migrate_unlock(dst_nid);

	if (autonuma_migrate_nr_pages)
		autonuma_printk("migrated %lu pages to node %d\n",
				autonuma_migrate_nr_pages, dst_nid);

	if (autonuma_balance_pgdat(pgdat, nr_pages))
		isolated = sync_isolate_migratepages(&migratepages,
						     page, pgdat,
						     migrated);

	if (isolated) {
		int err;
		pages_migrated += isolated; /* FIXME: per node */
		trace_numa_migratepages_begin(current->mm, &migratepages,
					      page_nid, dst_nid);
		err = migrate_pages(&migratepages, alloc_migrate_dst_page,
				    pgdat->node_id, false, MIGRATE_ASYNC);
		if (err)
			putback_lru_pages(&migratepages);
		trace_numa_migratepages_end(err);
	}
	BUG_ON(!list_empty(&migratepages));
out:
	return isolated;
}

static void cpu_follow_memory_pass(struct task_struct *p,
				   struct task_autonuma *task_autonuma,
				   unsigned long *task_numa_fault)
{
	int nid;
	/* If a new pass started, degrade the stats by a factor of 2 */
	for_each_node(nid)
		task_numa_fault[nid] >>= 1;
	task_autonuma->task_numa_fault_tot >>= 1;
}

static void numa_hinting_fault_cpu_follow_memory(struct task_struct *p,
						 int access_nid,
						 int numpages,
						 bool new_pass)
{
	struct task_autonuma *task_autonuma = p->task_autonuma;
	unsigned long *task_numa_fault = task_autonuma->task_numa_fault;

	/* prevent sched_autonuma_balance() to run on top of us */
	local_bh_disable();

	if (unlikely(new_pass))
		cpu_follow_memory_pass(p, task_autonuma, task_numa_fault);
	task_numa_fault[access_nid] += numpages;
	task_autonuma->task_numa_fault_tot += numpages;

	local_bh_enable();
}

/*
 * In this function we build a temporal CPU_node<->page relation by
 * using a two-stage autonuma_last_nid filter to remove short/unlikely
 * relations.
 *
 * Using P(p) ~ n_p / n_t as per frequentest probability, we can
 * equate a node's CPU usage of a particular page (n_p) per total
 * usage of this page (n_t) (in a given time-span) to a probability.
 *
 * Our periodic faults will then sample this probability and getting
 * the same result twice in a row, given these samples are fully
 * independent, is then given by P(n)^2, provided our sample period
 * is sufficiently short compared to the usage pattern.
 *
 * This quadric squishes small probabilities, making it less likely
 * we act on an unlikely CPU_node<->page relation.
 */
static inline bool last_nid_set(struct page *page, int this_nid)
{
	bool ret = true;
	struct page_autonuma *page_autonuma = lookup_page_autonuma(page);
	int autonuma_last_nid = ACCESS_ONCE(page_autonuma->autonuma_last_nid);
	VM_BUG_ON(this_nid < 0);
	VM_BUG_ON(this_nid >= MAX_NUMNODES);
	if (autonuma_last_nid != this_nid) {
		if (!autonuma_migrate_allow_first_fault() ||
		    autonuma_last_nid >= 0)
			ret = false;
		ACCESS_ONCE(page_autonuma->autonuma_last_nid) = this_nid;
	}
	return ret;
}

static int numa_hinting_fault_memory_follow_cpu(struct page *page,
						int this_nid, int page_nid,
						bool new_pass,
						bool *migrated)
{
	if (!last_nid_set(page, this_nid))
		goto out;
	if (!PageLRU(page))
		goto out;
	if (this_nid != page_nid) {
		if (autonuma_migrate_page(page, this_nid, page_nid,
					  migrated))
			return this_nid;
	}
out:
	return page_nid;
}

bool numa_hinting_fault(struct page *page, int numpages)
{
	bool migrated = false;

	/*
	 * "current->mm" could be different from the "mm" where the
	 * NUMA hinting page fault happened, if get_user_pages()
	 * triggered the fault on some other process "mm". That is ok,
	 * all we care about is to count the "page_nid" access on the
	 * current->task_autonuma, even if the page belongs to a
	 * different "mm".
	 */
	WARN_ON_ONCE(!current->mm);
	if (likely(current->mm && !current->mempolicy && autonuma_enabled())) {
		struct task_struct *p = current;
		int this_nid, page_nid, access_nid;
		bool new_pass;

		/*
		 * new_pass is only true the first time the thread
		 * faults on this pass of knuma_scand.
		 */
		new_pass = p->task_autonuma->task_numa_fault_pass !=
			p->mm->mm_autonuma->mm_numa_fault_pass;
		page_nid = page_to_nid(page);
		this_nid = numa_node_id();
		VM_BUG_ON(this_nid < 0);
		VM_BUG_ON(this_nid >= MAX_NUMNODES);
		access_nid = numa_hinting_fault_memory_follow_cpu(page,
								  this_nid,
								  page_nid,
								  new_pass,
								  &migrated);
		/* "page" has been already freed if "migrated" is true */
		numa_hinting_fault_cpu_follow_memory(p, access_nid,
						     numpages, new_pass);
		if (unlikely(new_pass))
			/*
			 * Set the task's fault_pass equal to the new
			 * mm's fault_pass, so new_pass will be false
			 * on the next fault by this thread in this
			 * same pass.
			 */
			p->task_autonuma->task_numa_fault_pass =
				p->mm->mm_autonuma->mm_numa_fault_pass;
	}

	return migrated;
}

/* NUMA hinting page fault entry point for ptes */
int pte_numa_fixup(struct mm_struct *mm, struct vm_area_struct *vma,
		   unsigned long addr, pte_t pte, pte_t *ptep, pmd_t *pmd)
{
	struct page *page;
	spinlock_t *ptl;
	bool migrated;

	/*
	 * The "pte" at this point cannot be used safely without
	 * validation through pte_unmap_same(). It's of NUMA type but
	 * the pfn may be screwed if the read is non atomic.
	 */

	ptl = pte_lockptr(mm, pmd);
	spin_lock(ptl);
	if (unlikely(!pte_same(*ptep, pte)))
		goto out_unlock;
	pte = pte_mknonnuma(pte);
	set_pte_at(mm, addr, ptep, pte);
	page = vm_normal_page(vma, addr, pte);
	BUG_ON(!page);
	if (unlikely(page_mapcount(page) != 1))
		goto out_unlock;
	get_page(page);
	pte_unmap_unlock(ptep, ptl);

	migrated = numa_hinting_fault(page, 1);
	if (!migrated)
		put_page(page);
out:
	return 0;

out_unlock:
	pte_unmap_unlock(ptep, ptl);
	goto out;
}

/* NUMA hinting page fault entry point for regular pmds */
int pmd_numa_fixup(struct mm_struct *mm, unsigned long addr, pmd_t *pmdp)
{
	pmd_t pmd;
	pte_t *pte, *orig_pte;
	unsigned long _addr = addr & PMD_MASK;
	unsigned long offset;
	spinlock_t *ptl;
	bool numa = false;
	struct vm_area_struct *vma;
	bool migrated;

	spin_lock(&mm->page_table_lock);
	pmd = *pmdp;
	if (pmd_numa(pmd)) {
		set_pmd_at(mm, _addr, pmdp, pmd_mknonnuma(pmd));
		numa = true;
	}
	spin_unlock(&mm->page_table_lock);

	if (!numa)
		return 0;

	vma = find_vma(mm, _addr);
	/* we're in a page fault so some vma must be in the range */
	BUG_ON(!vma);
	BUG_ON(vma->vm_start >= _addr + PMD_SIZE);
	offset = max(_addr, vma->vm_start) & ~PMD_MASK;
	VM_BUG_ON(offset >= PMD_SIZE);
	orig_pte = pte = pte_offset_map_lock(mm, pmdp, _addr, &ptl);
	pte += offset >> PAGE_SHIFT;
	for (addr = _addr + offset; addr < _addr + PMD_SIZE; pte++, addr += PAGE_SIZE) {
		pte_t pteval = *pte;
		struct page * page;
		if (!pte_present(pteval))
			continue;
		if (addr >= vma->vm_end) {
			vma = find_vma(mm, addr);
			/* there's a pte present so there must be a vma */
			BUG_ON(!vma);
			BUG_ON(addr < vma->vm_start);
		}
		if (pte_numa(pteval)) {
			pteval = pte_mknonnuma(pteval);
			set_pte_at(mm, addr, pte, pteval);
		}
		page = vm_normal_page(vma, addr, pteval);
		if (unlikely(!page))
			continue;
		/* only check non-shared pages */
		if (unlikely(page_mapcount(page) != 1))
			continue;
		get_page(page);
		pte_unmap_unlock(pte, ptl);

		migrated = numa_hinting_fault(page, 1);
		if (!migrated)
			put_page(page);

		pte = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	}
	pte_unmap_unlock(orig_pte, ptl);
	return 0;
}

static inline int task_autonuma_size(void)
{
	return sizeof(struct task_autonuma) +
		nr_node_ids * sizeof(unsigned long);
}

static inline int task_autonuma_reset_size(void)
{
	struct task_autonuma *task_autonuma = NULL;
	return task_autonuma_size() -
		(int)((char *)(&task_autonuma->task_numa_fault_pass) -
		      (char *)task_autonuma);
}

static void __task_autonuma_reset(struct task_autonuma *task_autonuma)
{
	memset(&task_autonuma->task_numa_fault_pass, 0,
	       task_autonuma_reset_size());
}

static void task_autonuma_reset(struct task_autonuma *task_autonuma)
{
	task_autonuma->task_selected_nid = -1;
	__task_autonuma_reset(task_autonuma);
}

static inline int mm_autonuma_fault_size(void)
{
	return nr_node_ids * sizeof(unsigned long);
}

static inline int mm_autonuma_size(void)
{
	return sizeof(struct mm_autonuma) + mm_autonuma_fault_size();
}

static inline int mm_autonuma_reset_size(void)
{
	struct mm_autonuma *mm_autonuma = NULL;
	return mm_autonuma_size() -
		(int)((char *)(&mm_autonuma->mm_numa_fault_pass) -
		      (char *)mm_autonuma);
}

static void mm_autonuma_reset(struct mm_autonuma *mm_autonuma)
{
	memset(&mm_autonuma->mm_numa_fault_pass, 0, mm_autonuma_reset_size());
}

void autonuma_setup_new_exec(struct task_struct *p)
{
	if (p->task_autonuma)
		task_autonuma_reset(p->task_autonuma);
	if (p->mm && p->mm->mm_autonuma)
		mm_autonuma_reset(p->mm->mm_autonuma);
}

static inline int knumad_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
}

/*
 * Here we search for not shared page mappings (mapcount == 1) and we
 * set up the pmd/pte_numa on those mappings so the very next access
 * will fire a NUMA hinting page fault. We also collect the
 * mm_autonuma statistics for this process mm at the same time.
 */
static int knuma_scand_pmd(struct mm_struct *mm,
			   struct vm_area_struct *vma,
			   unsigned long address)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte, *_pte;
	struct page *page;
	unsigned long _address, end;
	spinlock_t *ptl;
	int ret = 0;

	VM_BUG_ON(address & ~PAGE_MASK);

	pgd = pgd_offset(mm, address);
	if (!pgd_present(*pgd))
		goto out;

	pud = pud_offset(pgd, address);
	if (!pud_present(*pud))
		goto out;

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd))
		goto out;

	if (pmd_trans_huge_lock(pmd, vma) == 1) {
		int page_nid;
		unsigned long *fault_tmp;
		ret = HPAGE_PMD_NR;

		VM_BUG_ON(address & ~HPAGE_PMD_MASK);

		if (autonuma_mm_working_set() && pmd_numa(*pmd)) {
			spin_unlock(&mm->page_table_lock);
			goto out;
		}

		page = pmd_page(*pmd);

		/* only check non-shared pages */
		if (page_mapcount(page) != 1) {
			spin_unlock(&mm->page_table_lock);
			goto out;
		}

		page_nid = page_to_nid(page);
		fault_tmp = knuma_scand_data.mm_numa_fault_tmp;
		fault_tmp[page_nid] += ret;

		if (pmd_numa(*pmd)) {
			spin_unlock(&mm->page_table_lock);
			goto out;
		}

		set_pmd_at(mm, address, pmd, pmd_mknuma(*pmd));
		/* defer TLB flush to lower the overhead */
		spin_unlock(&mm->page_table_lock);
		goto out;
	}

	if (pmd_trans_unstable(pmd))
		goto out;
	VM_BUG_ON(!pmd_present(*pmd));

	end = min(vma->vm_end, (address + PMD_SIZE) & PMD_MASK);
	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	for (_address = address, _pte = pte; _address < end;
	     _pte++, _address += PAGE_SIZE) {
		pte_t pteval = *_pte;
		unsigned long *fault_tmp;
		if (!pte_present(pteval))
			continue;
		if (autonuma_mm_working_set() && pte_numa(pteval))
			continue;
		page = vm_normal_page(vma, _address, pteval);
		if (unlikely(!page))
			continue;
		/* only check non-shared pages */
		if (page_mapcount(page) != 1)
			continue;

		fault_tmp = knuma_scand_data.mm_numa_fault_tmp;
		fault_tmp[page_to_nid(page)]++;

		if (pte_numa(pteval))
			continue;

		if (!autonuma_scan_pmd())
			set_pte_at(mm, _address, _pte, pte_mknuma(pteval));

		/* defer TLB flush to lower the overhead */
		ret++;
	}
	pte_unmap_unlock(pte, ptl);

	if (ret && !pmd_numa(*pmd) && autonuma_scan_pmd()) {
		/*
		 * Mark the page table pmd as numa if "autonuma scan
		 * pmd" mode is enabled.
		 */
		spin_lock(&mm->page_table_lock);
		set_pmd_at(mm, address, pmd, pmd_mknuma(*pmd));
		spin_unlock(&mm->page_table_lock);
		/* defer TLB flush to lower the overhead */
	}

out:
	return ret;
}

static void mm_numa_fault_tmp_flush(struct mm_struct *mm)
{
	int nid;
	struct mm_autonuma *mma = mm->mm_autonuma;
	unsigned long tot;
	unsigned long *fault_tmp = knuma_scand_data.mm_numa_fault_tmp;

	if (autonuma_mm_working_set()) {
		for_each_node(nid) {
			tot = fault_tmp[nid];
			if (tot)
				break;
		}
		if (!tot)
			/* process was idle, keep the old data */
			return;
	}

	/* FIXME: would be better protected with write_seqlock_bh() */
	local_bh_disable();

	tot = 0;
	for_each_node(nid) {
		unsigned long faults = fault_tmp[nid];
		fault_tmp[nid] = 0;
		mma->mm_numa_fault[nid] = faults;
		tot += faults;
	}
	mma->mm_numa_fault_tot = tot;

	local_bh_enable();
}

static void mm_numa_fault_tmp_reset(void)
{
	memset(knuma_scand_data.mm_numa_fault_tmp, 0,
	       mm_autonuma_fault_size());
}

static inline void validate_mm_numa_fault_tmp(unsigned long address)
{
#ifdef CONFIG_DEBUG_VM
	int nid;
	if (address)
		return;
	for_each_node(nid)
		BUG_ON(knuma_scand_data.mm_numa_fault_tmp[nid]);
#endif
}

/*
 * Scan the next part of the mm. Keep track of the progress made and
 * return it.
 */
static int knumad_do_scan(void)
{
	struct mm_struct *mm;
	struct mm_autonuma *mm_autonuma;
	unsigned long address;
	struct vm_area_struct *vma;
	int progress = 0;

	mm = knuma_scand_data.mm;
	/*
	 * knuma_scand_data.mm is NULL after the end of each
	 * knuma_scand pass. So when it's NULL we've start from
	 * scratch from the very first mm in the list.
	 */
	if (!mm) {
		if (unlikely(list_empty(&knuma_scand_data.mm_head)))
			return pages_to_scan;
		mm_autonuma = list_entry(knuma_scand_data.mm_head.next,
					 struct mm_autonuma, mm_node);
		mm = mm_autonuma->mm;
		knuma_scand_data.address = 0;
		knuma_scand_data.mm = mm;
		atomic_inc(&mm->mm_count);
		mm_autonuma->mm_numa_fault_pass++;
	}
	address = knuma_scand_data.address;

	validate_mm_numa_fault_tmp(address);

	mutex_unlock(&knumad_mm_mutex);

	down_read(&mm->mmap_sem);
	if (unlikely(knumad_test_exit(mm)))
		vma = NULL;
	else
		vma = find_vma(mm, address);

	progress++;
	for (; vma && progress < pages_to_scan; vma = vma->vm_next) {
		unsigned long start_addr, end_addr;
		cond_resched();
		if (unlikely(knumad_test_exit(mm))) {
			progress++;
			break;
		}

		if (!vma->anon_vma || vma_policy(vma)) {
			progress++;
			continue;
		}
		if (vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP)) {
			progress++;
			continue;
		}
		/*
		 * Skip regions mprotected with PROT_NONE. It would be
		 * safe to scan them too, but it's worthless because
		 * NUMA hinting page faults can't run on those.
		 */
		if (!(vma->vm_flags & (VM_READ | VM_EXEC | VM_WRITE))) {
			progress++;
			continue;
		}
		if (is_vma_temporary_stack(vma)) {
			progress++;
			continue;
		}

		VM_BUG_ON(address & ~PAGE_MASK);
		if (address < vma->vm_start)
			address = vma->vm_start;

		start_addr = address;
		while (address < vma->vm_end) {
			cond_resched();
			if (unlikely(knumad_test_exit(mm)))
				break;

			VM_BUG_ON(address < vma->vm_start ||
				  address + PAGE_SIZE > vma->vm_end);
			progress += knuma_scand_pmd(mm, vma, address);
			/* move to next address */
			address = (address + PMD_SIZE) & PMD_MASK;
			if (progress >= pages_to_scan)
				break;
		}
		end_addr = min(address, vma->vm_end);

		/*
		 * Flush the TLB for the mm to start the NUMA hinting
		 * page faults after we finish scanning this vma part.
		 */
		mmu_notifier_invalidate_range_start(vma->vm_mm, start_addr,
						    end_addr);
		flush_tlb_range(vma, start_addr, end_addr);
		mmu_notifier_invalidate_range_end(vma->vm_mm, start_addr,
						  end_addr);
	}
	up_read(&mm->mmap_sem); /* exit_mmap will destroy ptes after this */

	mutex_lock(&knumad_mm_mutex);
	VM_BUG_ON(knuma_scand_data.mm != mm);
	knuma_scand_data.address = address;
	/*
	 * Change the current mm if this mm is about to die, or if we
	 * scanned all vmas of this mm.
	 */
	if (knumad_test_exit(mm) || !vma) {
		mm_autonuma = mm->mm_autonuma;
		if (mm_autonuma->mm_node.next != &knuma_scand_data.mm_head) {
			mm_autonuma = list_entry(mm_autonuma->mm_node.next,
						 struct mm_autonuma, mm_node);
			knuma_scand_data.mm = mm_autonuma->mm;
			atomic_inc(&knuma_scand_data.mm->mm_count);
			knuma_scand_data.address = 0;
			knuma_scand_data.mm->mm_autonuma->mm_numa_fault_pass++;
		} else
			knuma_scand_data.mm = NULL;

		if (knumad_test_exit(mm)) {
			list_del(&mm->mm_autonuma->mm_node);
			/* tell autonuma_exit not to list_del */
			VM_BUG_ON(mm->mm_autonuma->mm != mm);
			mm->mm_autonuma->mm = NULL;
			mm_numa_fault_tmp_reset();
		} else
			mm_numa_fault_tmp_flush(mm);

		mmdrop(mm);
	}

	return progress;
}

static void knuma_scand_disabled(void)
{
	if (!autonuma_enabled())
		wait_event_freezable(knuma_scand_wait,
				     autonuma_enabled() ||
				     kthread_should_stop());
}

static int knuma_scand(void *none)
{
	struct mm_struct *mm = NULL;
	int progress = 0, _progress;
	unsigned long total_progress = 0;

	set_freezable();

	knuma_scand_disabled();

	/*
	 * Serialize the knuma_scand_data against
	 * autonuma_enter/exit().
	 */
	mutex_lock(&knumad_mm_mutex);

	for (;;) {
		if (unlikely(kthread_should_stop()))
			break;

		/* Do one loop of scanning, keeping track of the progress */
		_progress = knumad_do_scan();
		progress += _progress;
		total_progress += _progress;
		mutex_unlock(&knumad_mm_mutex);

		/* Check if we completed one full scan pass */
		if (unlikely(!knuma_scand_data.mm)) {
			autonuma_printk("knuma_scand %lu\n", total_progress);
			pages_scanned += total_progress;
			total_progress = 0;
			full_scans++;

			wait_event_freezable_timeout(knuma_scand_wait,
						     kthread_should_stop(),
						     msecs_to_jiffies(
						     scan_sleep_pass_millisecs));

			if (autonuma_debug()) {
				extern void sched_autonuma_dump_mm(void);
				sched_autonuma_dump_mm();
			}

			/* wait while there is no pinned mm */
			knuma_scand_disabled();
		}
		if (progress > pages_to_scan) {
			progress = 0;
			wait_event_freezable_timeout(knuma_scand_wait,
						     kthread_should_stop(),
						     msecs_to_jiffies(
						     scan_sleep_millisecs));
		}
		cond_resched();
		mutex_lock(&knumad_mm_mutex);
	}

	mm = knuma_scand_data.mm;
	knuma_scand_data.mm = NULL;
	if (mm && knumad_test_exit(mm)) {
		list_del(&mm->mm_autonuma->mm_node);
		/* tell autonuma_exit not to list_del */
		VM_BUG_ON(mm->mm_autonuma->mm != mm);
		mm->mm_autonuma->mm = NULL;
	}
	mutex_unlock(&knumad_mm_mutex);

	if (mm)
		mmdrop(mm);
	mm_numa_fault_tmp_reset();

	return 0;
}

void autonuma_enter(struct mm_struct *mm)
{
	if (!autonuma_possible())
		return;

	mutex_lock(&knumad_mm_mutex);
	list_add_tail(&mm->mm_autonuma->mm_node, &knuma_scand_data.mm_head);
	mutex_unlock(&knumad_mm_mutex);
}

void autonuma_exit(struct mm_struct *mm)
{
	bool serialize;

	if (!autonuma_possible())
		return;

	serialize = false;
	mutex_lock(&knumad_mm_mutex);
	if (knuma_scand_data.mm == mm)
		serialize = true;
	else if (mm->mm_autonuma->mm) {
		VM_BUG_ON(mm->mm_autonuma->mm != mm);
		mm->mm_autonuma->mm = NULL; /* debug */
		list_del(&mm->mm_autonuma->mm_node);
	}
	mutex_unlock(&knumad_mm_mutex);

	if (serialize) {
		/* prevent the mm to go away under knumad_do_scan main loop */
		down_write(&mm->mmap_sem);
		up_write(&mm->mmap_sem);
	}
}

static int start_knuma_scand(void)
{
	int err = 0;
	struct task_struct *knumad_thread;

	knuma_scand_data.mm_numa_fault_tmp = kzalloc(mm_autonuma_fault_size(),
						     GFP_KERNEL);
	if (!knuma_scand_data.mm_numa_fault_tmp)
		return -ENOMEM;

	knumad_thread = kthread_run(knuma_scand, NULL, "knuma_scand");
	if (unlikely(IS_ERR(knumad_thread))) {
		autonuma_printk(KERN_ERR
				"knumad: kthread_run(knuma_scand) failed\n");
		err = PTR_ERR(knumad_thread);
	}
	return err;
}


#ifdef CONFIG_SYSFS

static ssize_t flag_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf,
			 enum autonuma_flag flag)
{
	return sprintf(buf, "%d\n",
		       !!test_bit(flag, &autonuma_flags));
}
static ssize_t flag_store(struct kobject *kobj,
			  struct kobj_attribute *attr,
			  const char *buf, size_t count,
			  enum autonuma_flag flag)
{
	unsigned long value;
	int ret;

	ret = kstrtoul(buf, 10, &value);
	if (ret < 0)
		return ret;
	if (value > 1)
		return -EINVAL;

	if (value)
		set_bit(flag, &autonuma_flags);
	else
		clear_bit(flag, &autonuma_flags);

	return count;
}

static ssize_t enabled_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return flag_show(kobj, attr, buf, AUTONUMA_ENABLED_FLAG);
}
static ssize_t enabled_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	ssize_t ret;

	ret = flag_store(kobj, attr, buf, count, AUTONUMA_ENABLED_FLAG);

	if (ret > 0 && autonuma_enabled())
		wake_up_interruptible(&knuma_scand_wait);

	return ret;
}
static struct kobj_attribute enabled_attr =
	__ATTR(enabled, 0644, enabled_show, enabled_store);

#define SYSFS_ENTRY(NAME, FLAG)						\
static ssize_t NAME ## _show(struct kobject *kobj,			\
			     struct kobj_attribute *attr, char *buf)	\
{									\
	return flag_show(kobj, attr, buf, FLAG);			\
}									\
									\
static ssize_t NAME ## _store(struct kobject *kobj,			\
			      struct kobj_attribute *attr,		\
			      const char *buf, size_t count)		\
{									\
	return flag_store(kobj, attr, buf, count, FLAG);		\
}									\
static struct kobj_attribute NAME ## _attr =				\
	__ATTR(NAME, 0644, NAME ## _show, NAME ## _store);

SYSFS_ENTRY(scan_pmd, AUTONUMA_SCAN_PMD_FLAG);
SYSFS_ENTRY(debug, AUTONUMA_DEBUG_FLAG);
#ifdef CONFIG_DEBUG_VM
SYSFS_ENTRY(sched_load_balance_strict, AUTONUMA_SCHED_LOAD_BALANCE_STRICT_FLAG);
SYSFS_ENTRY(child_inheritance, AUTONUMA_CHILD_INHERITANCE_FLAG);
SYSFS_ENTRY(migrate_allow_first_fault,
	    AUTONUMA_MIGRATE_ALLOW_FIRST_FAULT_FLAG);
SYSFS_ENTRY(mm_working_set, AUTONUMA_MM_WORKING_SET_FLAG);
#endif /* CONFIG_DEBUG_VM */

#undef SYSFS_ENTRY

enum {
	SYSFS_SCAN_SLEEP_ENTRY,
	SYSFS_SCAN_PAGES_ENTRY,
	SYSFS_MIGRATE_SLEEP_ENTRY,
	SYSFS_MIGRATE_PAGES_ENTRY,
};

#define SYSFS_ENTRY(NAME, SYSFS_TYPE)					\
	static ssize_t NAME ## _show(struct kobject *kobj,		\
				     struct kobj_attribute *attr,	\
				     char *buf)				\
	{								\
		return sprintf(buf, "%u\n", NAME);			\
	}								\
	static ssize_t NAME ## _store(struct kobject *kobj,		\
				      struct kobj_attribute *attr,	\
				      const char *buf, size_t count)	\
	{								\
		unsigned long val;					\
		int err;						\
									\
		err = strict_strtoul(buf, 10, &val);			\
		if (err || val > UINT_MAX)				\
			return -EINVAL;					\
		switch (SYSFS_TYPE) {					\
		case SYSFS_SCAN_PAGES_ENTRY:				\
		case SYSFS_MIGRATE_PAGES_ENTRY:				\
			if (!val)					\
				return -EINVAL;				\
			break;						\
		}							\
									\
		NAME = val;						\
		switch (SYSFS_TYPE) {					\
		case SYSFS_SCAN_SLEEP_ENTRY:				\
			wake_up_interruptible(&knuma_scand_wait);	\
			break;						\
		}							\
									\
		return count;						\
	}								\
	static struct kobj_attribute NAME ## _attr =			\
		__ATTR(NAME, 0644, NAME ## _show, NAME ## _store);

SYSFS_ENTRY(scan_sleep_millisecs, SYSFS_SCAN_SLEEP_ENTRY);
SYSFS_ENTRY(scan_sleep_pass_millisecs, SYSFS_SCAN_SLEEP_ENTRY);
SYSFS_ENTRY(pages_to_scan, SYSFS_SCAN_PAGES_ENTRY);

SYSFS_ENTRY(migrate_sleep_millisecs, SYSFS_MIGRATE_SLEEP_ENTRY);
SYSFS_ENTRY(pages_to_migrate, SYSFS_MIGRATE_PAGES_ENTRY);

#undef SYSFS_ENTRY

#define SYSFS_ENTRY(NAME)					\
static ssize_t NAME ## _show(struct kobject *kobj,		\
			     struct kobj_attribute *attr,	\
			     char *buf)				\
{								\
	return sprintf(buf, "%lu\n", NAME);			\
}								\
static struct kobj_attribute NAME ## _attr =			\
	__ATTR_RO(NAME);

SYSFS_ENTRY(full_scans);
SYSFS_ENTRY(pages_scanned);
SYSFS_ENTRY(pages_migrated);

#undef SYSFS_ENTRY

static struct attribute *autonuma_attr[] = {
	&enabled_attr.attr,

	&debug_attr.attr,

	/* migrate start */
	&migrate_sleep_millisecs_attr.attr,
	&pages_to_migrate_attr.attr,
	&pages_migrated_attr.attr,
#ifdef CONFIG_DEBUG_VM
	&migrate_allow_first_fault_attr.attr,
#endif
	/* migrate end */

	/* scan start */
	&scan_sleep_millisecs_attr.attr,
	&scan_sleep_pass_millisecs_attr.attr,
	&pages_to_scan_attr.attr,
	&pages_scanned_attr.attr,
	&full_scans_attr.attr,
	&scan_pmd_attr.attr,
	/* scan end */

#ifdef CONFIG_DEBUG_VM
	&sched_load_balance_strict_attr.attr,
	&child_inheritance_attr.attr,
	&mm_working_set_attr.attr,
#endif

	NULL,
};
static struct attribute_group autonuma_attr_group = {
	.attrs = autonuma_attr,
};

static int __init autonuma_init_sysfs(struct kobject **autonuma_kobj)
{
	int err;

	*autonuma_kobj = kobject_create_and_add("autonuma", mm_kobj);
	if (unlikely(!*autonuma_kobj)) {
		printk(KERN_ERR "autonuma: failed kobject create\n");
		return -ENOMEM;
	}

	err = sysfs_create_group(*autonuma_kobj, &autonuma_attr_group);
	if (err) {
		printk(KERN_ERR "autonuma: failed register autonuma group\n");
		goto delete_obj;
	}

	return 0;

delete_obj:
	kobject_put(*autonuma_kobj);
	return err;
}

static void __init autonuma_exit_sysfs(struct kobject *autonuma_kobj)
{
	sysfs_remove_group(autonuma_kobj, &autonuma_attr_group);
	kobject_put(autonuma_kobj);
}
#else
static inline int autonuma_init_sysfs(struct kobject **autonuma_kobj)
{
	return 0;
}

static inline void autonuma_exit_sysfs(struct kobject *autonuma_kobj)
{
}
#endif /* CONFIG_SYSFS */

static int __init noautonuma_setup(char *str)
{
	if (autonuma_possible()) {
		printk("AutoNUMA permanently disabled\n");
		clear_bit(AUTONUMA_POSSIBLE_FLAG, &autonuma_flags);
		WARN_ON(autonuma_possible()); /* avoid early crash */
	}
	return 1;
}
/* early so sparse.c also can see it */
early_param("noautonuma", noautonuma_setup);

static bool autonuma_init_checks_failed(void)
{
	/* safety checks on nr_node_ids */
	int last_nid = find_last_bit(node_states[N_POSSIBLE].bits, MAX_NUMNODES);
	if (last_nid + 1 != nr_node_ids) {
		WARN_ON(1);
		return true;
	}
	if (num_possible_nodes() > nr_node_ids) {
		WARN_ON(1);
		return true;
	}
	return false;
}

static int __init autonuma_init(void)
{
	int err;
	struct kobject *autonuma_kobj;

	VM_BUG_ON(num_possible_nodes() < 1);
	if (num_possible_nodes() <= 1 || !autonuma_possible()) {
		/* should have been already initialized by page_autonuma */
		if (autonuma_possible()) {
			WARN_ON(1);
			/* try to fixup if it wasn't ok */
			clear_bit(AUTONUMA_POSSIBLE_FLAG, &autonuma_flags);
		}
		return -EINVAL;
	} else if (autonuma_init_checks_failed()) {
		printk("autonuma disengaged: init checks failed\n");
		clear_bit(AUTONUMA_POSSIBLE_FLAG, &autonuma_flags);
		return -EINVAL;
	}

	err = autonuma_init_sysfs(&autonuma_kobj);
	if (err)
		return err;

	err = start_knuma_scand();
	if (err) {
		printk("failed to start knuma_scand\n");
		goto out;
	}

	printk("AutoNUMA initialized successfully\n");
	return err;

out:
	autonuma_exit_sysfs(autonuma_kobj);
	return err;
}
module_init(autonuma_init)

static struct kmem_cache *task_autonuma_cachep;

int alloc_task_autonuma(struct task_struct *tsk, struct task_struct *orig,
			 int node)
{
	int err = 1;
	struct task_autonuma *task_autonuma;

	if (!autonuma_possible())
		goto no_numa;
	task_autonuma = kmem_cache_alloc_node(task_autonuma_cachep,
					      GFP_KERNEL, node);
	if (!task_autonuma)
		goto out;
	if (!autonuma_child_inheritance()) {
		/*
		 * Only reset the task NUMA stats, Always inherit the
		 * task_selected_nid. It's certainly better to start
		 * the child in the same NUMA node of the parent, if
		 * idle/load balancing permits. If they don't permit,
		 * task_selected_nid is a transient entity and it'll
		 * be updated accordingly.
		 */
		task_autonuma->task_selected_nid =
			orig->task_autonuma->task_selected_nid;
		__task_autonuma_reset(task_autonuma);
	} else
		memcpy(task_autonuma, orig->task_autonuma,
		       task_autonuma_size());
	VM_BUG_ON(task_autonuma->task_selected_nid < -1);
	VM_BUG_ON(task_autonuma->task_selected_nid >= nr_node_ids);
	tsk->task_autonuma = task_autonuma;
no_numa:
	err = 0;
out:
	return err;
}

void free_task_autonuma(struct task_struct *tsk)
{
	if (!autonuma_possible()) {
		BUG_ON(tsk->task_autonuma);
		return;
	}

	BUG_ON(!tsk->task_autonuma);
	kmem_cache_free(task_autonuma_cachep, tsk->task_autonuma);
	tsk->task_autonuma = NULL;
}

void __init task_autonuma_init(void)
{
	struct task_autonuma *task_autonuma;

	BUG_ON(current != &init_task);

	if (!autonuma_possible())
		return;

	task_autonuma_cachep =
		kmem_cache_create("task_autonuma",
				  task_autonuma_size(), 0,
				  SLAB_PANIC | SLAB_HWCACHE_ALIGN, NULL);

	task_autonuma = kmem_cache_alloc_node(task_autonuma_cachep,
					      GFP_KERNEL, numa_node_id());
	BUG_ON(!task_autonuma);
	task_autonuma_reset(task_autonuma);
	BUG_ON(current->task_autonuma);
	current->task_autonuma = task_autonuma;
}

static struct kmem_cache *mm_autonuma_cachep;

int alloc_mm_autonuma(struct mm_struct *mm)
{
	int err = 1;
	struct mm_autonuma *mm_autonuma;

	if (!autonuma_possible())
		goto no_numa;
	mm_autonuma = kmem_cache_alloc(mm_autonuma_cachep, GFP_KERNEL);
	if (!mm_autonuma)
		goto out;
	if (!autonuma_child_inheritance() || !mm->mm_autonuma)
		mm_autonuma_reset(mm_autonuma);
	else
		memcpy(mm_autonuma, mm->mm_autonuma, mm_autonuma_size());

	/*
	 * We're not leaking memory here, if mm->mm_autonuma is not
	 * zero it's a not refcounted copy of the parent's
	 * mm->mm_autonuma pointer.
	 */
	mm->mm_autonuma = mm_autonuma;
	mm_autonuma->mm = mm;
no_numa:
	err = 0;
out:
	return err;
}

void free_mm_autonuma(struct mm_struct *mm)
{
	if (!autonuma_possible()) {
		BUG_ON(mm->mm_autonuma);
		return;
	}

	BUG_ON(!mm->mm_autonuma);
	kmem_cache_free(mm_autonuma_cachep, mm->mm_autonuma);
	mm->mm_autonuma = NULL;
}

void __init mm_autonuma_init(void)
{
	BUG_ON(current != &init_task);
	BUG_ON(current->mm);

	if (!autonuma_possible())
		return;

	mm_autonuma_cachep =
		kmem_cache_create("mm_autonuma",
				  mm_autonuma_size(), 0,
				  SLAB_PANIC | SLAB_HWCACHE_ALIGN, NULL);
}
