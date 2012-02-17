/*
 *  Copyright (C) 2012  Red Hat, Inc.
 *
 *  This work is licensed under the terms of the GNU GPL, version 2. See
 *  the COPYING file in the top-level directory.
 *
 *  Boot with "numa=fake=2" to test on not NUMA systems.
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

unsigned long autonuma_flags __read_mostly =
	(1<<AUTONUMA_SCHED_LOAD_BALANCE_STRICT_FLAG)|
	(1<<AUTONUMA_SCHED_CLONE_RESET_FLAG)|
	(1<<AUTONUMA_SCHED_FORK_RESET_FLAG)|
#ifdef CONFIG_AUTONUMA_DEFAULT_ENABLED
	(1<<AUTONUMA_FLAG)|
#endif
	(1<<AUTONUMA_SCAN_PMD_FLAG);

static DEFINE_MUTEX(knumad_mm_mutex);

/* knuma_scand */
static unsigned int scan_sleep_millisecs __read_mostly = 100;
static unsigned int scan_sleep_pass_millisecs __read_mostly = 5000;
static unsigned int pages_to_scan __read_mostly = 128*1024*1024/PAGE_SIZE;
static DECLARE_WAIT_QUEUE_HEAD(knuma_scand_wait);
static unsigned long full_scans;
static unsigned long pages_scanned;

/* knuma_migrated */
static unsigned int migrate_sleep_millisecs __read_mostly = 100;
static unsigned int pages_to_migrate __read_mostly = 128*1024*1024/PAGE_SIZE;
static volatile unsigned long pages_migrated;

static struct knumad_scan {
	struct list_head mm_head;
	struct mm_struct *mm;
	unsigned long address;
} knumad_scan = {
	.mm_head = LIST_HEAD_INIT(knumad_scan.mm_head),
};

static inline bool autonuma_impossible(void)
{
	return num_possible_nodes() <= 1 ||
		test_bit(AUTONUMA_IMPOSSIBLE, &autonuma_flags);
}

static inline void autonuma_migrate_lock(int nid)
{
	spin_lock(&NODE_DATA(nid)->autonuma_lock);
}

static inline void autonuma_migrate_unlock(int nid)
{
	spin_unlock(&NODE_DATA(nid)->autonuma_lock);
}

static inline void autonuma_migrate_lock_irq(int nid)
{
	spin_lock_irq(&NODE_DATA(nid)->autonuma_lock);
}

static inline void autonuma_migrate_unlock_irq(int nid)
{
	spin_unlock_irq(&NODE_DATA(nid)->autonuma_lock);
}

/* caller already holds the compound_lock */
void autonuma_migrate_split_huge_page(struct page *page,
				      struct page *page_tail)
{
	int nid;

	nid = page->autonuma_migrate_nid;
	VM_BUG_ON(nid >= MAX_NUMNODES);
	VM_BUG_ON(nid < -1);
	VM_BUG_ON(page_tail->autonuma_migrate_nid != -1);
	if (nid >= 0) {
		VM_BUG_ON(page_to_nid(page) != page_to_nid(page_tail));
		autonuma_migrate_lock(nid);
		list_add_tail(&page_tail->autonuma_migrate_node,
			      &page->autonuma_migrate_node);
		autonuma_migrate_unlock(nid);

		page_tail->autonuma_migrate_nid = nid;
	}
}

void __autonuma_migrate_page_remove(struct page *page)
{
	unsigned long flags;
	int nid;

	flags = compound_lock_irqsave(page);

	nid = page->autonuma_migrate_nid;
	VM_BUG_ON(nid >= MAX_NUMNODES);
	VM_BUG_ON(nid < -1);
	if (nid >= 0) {
		int numpages = hpage_nr_pages(page);
		autonuma_migrate_lock(nid);
		list_del(&page->autonuma_migrate_node);
		NODE_DATA(nid)->autonuma_nr_migrate_pages -= numpages;
		autonuma_migrate_unlock(nid);

		page->autonuma_migrate_nid = -1;
	}

	compound_unlock_irqrestore(page, flags);
}

static void __autonuma_migrate_page_add(struct page *page, int dst_nid,
					int page_nid)
{
	unsigned long flags;
	int nid;
	int numpages;
	unsigned long nr_migrate_pages;
	wait_queue_head_t *wait_queue;

	VM_BUG_ON(dst_nid >= MAX_NUMNODES);
	VM_BUG_ON(dst_nid < -1);
	VM_BUG_ON(page_nid >= MAX_NUMNODES);
	VM_BUG_ON(page_nid < -1);

	VM_BUG_ON(page_nid == dst_nid);
	VM_BUG_ON(page_to_nid(page) != page_nid);

	flags = compound_lock_irqsave(page);

	numpages = hpage_nr_pages(page);
	nid = page->autonuma_migrate_nid;
	VM_BUG_ON(nid >= MAX_NUMNODES);
	VM_BUG_ON(nid < -1);
	if (nid >= 0) {
		autonuma_migrate_lock(nid);
		list_del(&page->autonuma_migrate_node);
		NODE_DATA(nid)->autonuma_nr_migrate_pages -= numpages;
		autonuma_migrate_unlock(nid);
	}

	autonuma_migrate_lock(dst_nid);
	list_add(&page->autonuma_migrate_node,
		 &NODE_DATA(dst_nid)->autonuma_migrate_head[page_nid]);
	NODE_DATA(dst_nid)->autonuma_nr_migrate_pages += numpages;
	nr_migrate_pages = NODE_DATA(dst_nid)->autonuma_nr_migrate_pages;

	autonuma_migrate_unlock(dst_nid);

	page->autonuma_migrate_nid = dst_nid;

	compound_unlock_irqrestore(page, flags);

	if (!autonuma_migrate_defer()) {
		wait_queue = &NODE_DATA(dst_nid)->autonuma_knuma_migrated_wait;
		if (nr_migrate_pages >= pages_to_migrate &&
		    nr_migrate_pages - numpages < pages_to_migrate &&
		    waitqueue_active(wait_queue))
			wake_up_interruptible(wait_queue);
	}
}

static void autonuma_migrate_page_add(struct page *page, int dst_nid,
				      int page_nid)
{
	int migrate_nid = ACCESS_ONCE(page->autonuma_migrate_nid);
	if (migrate_nid != dst_nid)
		__autonuma_migrate_page_add(page, dst_nid, page_nid);
}

static bool balance_pgdat(struct pglist_data *pgdat,
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
		 * FIXME: deal with order with THP, maybe if
		 * kswapd will learn using compaction, otherwise
		 * order = 0 probably is ok.
		 * FIXME: in theory we're ok if we can obtain
		 * pages_to_migrate pages from all zones, it doesn't
		 * need to be all in a single zone. We care about the
		 * pgdat, the zone not.
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

static void cpu_follow_memory_pass(struct task_struct *p,
				   struct sched_autonuma *sched_autonuma,
				   unsigned long *numa_fault)
{
	int nid;
#if 0
	if (sched_autonuma->numa_fault_tot >
	    p->mm->mm_autonuma->numa_fault_tot) {
		for_each_node(nid)
			numa_fault[nid] >>= 1;
		sched_autonuma->numa_fault_tot >>= 1;
	}
#else
	for_each_node(nid) {
#if 0
		autonuma_printk("mm %p pid %d nid %d %lu\n", p->mm, p->pid,
				nid, numa_fault[nid]);
#endif
		numa_fault[nid] >>= 1;
	}
#if 0
	autonuma_printk("mm %p pid %d total %lu autonuma %p fault %p cpu %d\n",
			p->mm, p->pid,
			sched_autonuma->numa_fault_tot, sched_autonuma,
			numa_fault,
			smp_processor_id());
#endif
	sched_autonuma->numa_fault_tot >>= 1;
#endif
}

static void numa_hinting_fault_cpu_follow_memory(struct task_struct *p,
						 int access_nid,
						 int numpages,
						 bool pass)
{
	struct sched_autonuma *sched_autonuma = p->sched_autonuma;
	unsigned long *numa_fault = sched_autonuma->numa_fault;
	if (unlikely(pass))
		cpu_follow_memory_pass(p, sched_autonuma, numa_fault);
	numa_fault[access_nid] += numpages;
	sched_autonuma->numa_fault_tot += numpages;
}

static inline bool last_nid_set(struct task_struct *p,
				struct page *page, int cpu_nid)
{
	bool ret = true;
	int autonuma_last_nid = ACCESS_ONCE(page->autonuma_last_nid);
	VM_BUG_ON(cpu_nid < 0);
	VM_BUG_ON(cpu_nid >= MAX_NUMNODES);
	if (autonuma_last_nid >= 0 && autonuma_last_nid != cpu_nid) {
		int migrate_nid = ACCESS_ONCE(page->autonuma_migrate_nid);
		if (migrate_nid >= 0 && migrate_nid != cpu_nid)
			__autonuma_migrate_page_remove(page);
		ret = false;
	}
	if (autonuma_last_nid != cpu_nid)
		ACCESS_ONCE(page->autonuma_last_nid) = cpu_nid;
	return ret;
}

static int __page_migrate_nid(struct page *page, int page_nid)
{
	int migrate_nid = ACCESS_ONCE(page->autonuma_migrate_nid);
	if (migrate_nid < 0)
		migrate_nid = page_nid;
#if 0
	return page_nid;
#endif
	return migrate_nid;
}

static int page_migrate_nid(struct page *page)
{
	return __page_migrate_nid(page, page_to_nid(page));
}

static int numa_hinting_fault_memory_follow_cpu(struct task_struct *p,
						struct page *page,
						int cpu_nid, int page_nid,
						bool pass)
{
	if (!last_nid_set(p, page, cpu_nid))
		return __page_migrate_nid(page, page_nid);
	if (!PageLRU(page))
		return page_nid;
	if (cpu_nid != page_nid)
		autonuma_migrate_page_add(page, cpu_nid, page_nid);
	else
		autonuma_migrate_page_remove(page);
	return cpu_nid;
}

void numa_hinting_fault(struct page *page, int numpages)
{
	WARN_ON_ONCE(!current->mm);
	if (likely(current->mm && autonuma_enabled())) {
		struct task_struct *p = current;
		int cpu_nid, page_nid, access_nid;
		bool pass;

		pass = p->sched_autonuma->numa_fault_pass !=
			p->mm->mm_autonuma->numa_fault_pass;
		page_nid = page_to_nid(page);
		cpu_nid = numa_node_id();
		VM_BUG_ON(cpu_nid < 0);
		VM_BUG_ON(cpu_nid >= MAX_NUMNODES);
		access_nid = numa_hinting_fault_memory_follow_cpu(p, page,
								  cpu_nid,
								  page_nid,
								  pass);
		numa_hinting_fault_cpu_follow_memory(p, access_nid,
						     numpages, pass);
		if (unlikely(pass))
			p->sched_autonuma->numa_fault_pass =
				p->mm->mm_autonuma->numa_fault_pass;
	}
}

pte_t __pte_numa_fixup(struct mm_struct *mm, struct vm_area_struct *vma,
		       unsigned long addr, pte_t pte, pte_t *ptep)
{
	struct page *page;
	pte = pte_mknotnuma(pte);
	set_pte_at(mm, addr, ptep, pte);
	page = vm_normal_page(vma, addr, pte);
	BUG_ON(!page);
	numa_hinting_fault(page, 1);
	return pte;
}

void __pmd_numa_fixup(struct mm_struct *mm, struct vm_area_struct *vma,
		      unsigned long addr, pmd_t *pmdp)
{
	pmd_t pmd;
	pte_t *pte;
	unsigned long _addr = addr & PMD_MASK;
	spinlock_t *ptl;
	bool numa = false;

	spin_lock(&mm->page_table_lock);
	pmd = *pmdp;
	if (pmd_numa(pmd)) {
		set_pmd_at(mm, _addr, pmdp, pmd_mknotnuma(pmd));
		numa = true;
	}
	spin_unlock(&mm->page_table_lock);

	if (!numa)
		return;

	pte = pte_offset_map_lock(mm, pmdp, _addr, &ptl);
	for (addr = _addr; addr < _addr + PMD_SIZE; pte++, addr += PAGE_SIZE) {
		pte_t pteval = *pte;
		struct page * page;
		if (!pte_present(pteval))
			continue;
		if (pte_numa(pteval)) {
			pteval = pte_mknotnuma(pteval);
			set_pte_at(mm, addr, pte, pteval);
		}
		page = vm_normal_page(vma, addr, pteval);
		if (unlikely(!page))
			continue;
		/* only check non-shared pages */
		if (page_mapcount(page) != 1)
			continue;
		numa_hinting_fault(page, 1);
	}
	pte_unmap_unlock(pte, ptl);
}

static inline int sched_autonuma_size(void)
{
	return sizeof(struct sched_autonuma) +
		num_possible_nodes() * sizeof(unsigned long);
}

static inline int sched_autonuma_reset_size(void)
{
	struct sched_autonuma *sched_autonuma = NULL;
	return sched_autonuma_size() -
		(int)((char *)(&sched_autonuma->autonuma_stop_one_cpu) -
		      (char *)sched_autonuma);
}

static void sched_autonuma_reset(struct sched_autonuma *sched_autonuma)
{
	sched_autonuma->autonuma_node = -1;
	memset(&sched_autonuma->autonuma_stop_one_cpu, 0,
	       sched_autonuma_reset_size());
}

static inline int mm_autonuma_fault_size(void)
{
	return num_possible_nodes() * sizeof(unsigned long);
}

static inline unsigned long *mm_autonuma_numa_fault_tmp(struct mm_struct *mm)
{
	return mm->mm_autonuma->numa_fault + num_possible_nodes();
}

static inline int mm_autonuma_size(void)
{
	return sizeof(struct mm_autonuma) + mm_autonuma_fault_size() * 2;
}

static inline int mm_autonuma_reset_size(void)
{
	struct mm_autonuma *mm_autonuma = NULL;
	return mm_autonuma_size() -
		(int)((char *)(&mm_autonuma->numa_fault_tot) -
		      (char *)mm_autonuma);
}

static void mm_autonuma_reset(struct mm_autonuma *mm_autonuma)
{
	memset(&mm_autonuma->numa_fault_tot, 0, mm_autonuma_reset_size());
}

void autonuma_setup_new_exec(struct task_struct *p)
{
	if (p->sched_autonuma)
		sched_autonuma_reset(p->sched_autonuma);
	if (p->mm && p->mm->mm_autonuma)
		mm_autonuma_reset(p->mm->mm_autonuma);
}

static inline int knumad_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
}

static int knumad_scan_pmd(struct mm_struct *mm,
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
	if (pmd_trans_huge(*pmd)) {
		spin_lock(&mm->page_table_lock);
		if (pmd_trans_huge(*pmd)) {
			VM_BUG_ON(address & ~HPAGE_PMD_MASK);
			if (unlikely(pmd_trans_splitting(*pmd))) {
				spin_unlock(&mm->page_table_lock);
				wait_split_huge_page(vma->anon_vma, pmd);
			} else {
				int page_nid;
				unsigned long *numa_fault_tmp;
				ret = HPAGE_PMD_NR;

				if (autonuma_scan_use_working_set() &&
				    pmd_numa(*pmd)) {
					spin_unlock(&mm->page_table_lock);
					goto out;
				}

				page = pmd_page(*pmd);

				/* only check non-shared pages */
				if (page_mapcount(page) != 1) {
					spin_unlock(&mm->page_table_lock);
					goto out;
				}

				page_nid = page_migrate_nid(page);
				numa_fault_tmp = mm_autonuma_numa_fault_tmp(mm);
				numa_fault_tmp[page_nid] += ret;

				if (pmd_numa(*pmd)) {
					spin_unlock(&mm->page_table_lock);
					goto out;
				}

				set_pmd_at(mm, address, pmd, pmd_mknuma(*pmd));
				/* defer TLB flush to lower the overhead */
				spin_unlock(&mm->page_table_lock);
				goto out;
			}
		}
	}

	VM_BUG_ON(!pmd_present(*pmd));

	end = min(vma->vm_end, (address + PMD_SIZE) & PMD_MASK);
	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	for (_address = address, _pte = pte; _address < end;
	     _pte++, _address += PAGE_SIZE) {
		unsigned long *numa_fault_tmp;
		pte_t pteval = *_pte;
		if (!pte_present(pteval))
			continue;
		if (autonuma_scan_use_working_set() &&
		    pte_numa(pteval))
			continue;
		page = vm_normal_page(vma, _address, pteval);
		if (unlikely(!page))
			continue;
		/* only check non-shared pages */
		if (page_mapcount(page) != 1)
			continue;

		numa_fault_tmp = mm_autonuma_numa_fault_tmp(mm);
		numa_fault_tmp[page_migrate_nid(page)]++;

		if (pte_numa(pteval))
			continue;

		if (!autonuma_scan_pmd())
			set_pte_at(mm, _address, _pte, pte_mknuma(pteval));

		/* defer TLB flush to lower the overhead */
		ret++;
	}
	pte_unmap_unlock(pte, ptl);

	if (ret && !pmd_numa(*pmd) && autonuma_scan_pmd()) {
		spin_lock(&mm->page_table_lock);
		set_pmd_at(mm, address, pmd, pmd_mknuma(*pmd));
		spin_unlock(&mm->page_table_lock);
		/* defer TLB flush to lower the overhead */
	}

out:
	return ret;
}

static void mm_numa_fault_flush(struct mm_struct *mm)
{
	int nid;
	struct mm_autonuma *mma = mm->mm_autonuma;
	unsigned long *numa_fault_tmp = mm_autonuma_numa_fault_tmp(mm);
	unsigned long tot = 0;
	/* FIXME: protect this with seqlock against autonuma_balance() */
	for_each_node(nid) {
		mma->numa_fault[nid] = numa_fault_tmp[nid];
		tot += mma->numa_fault[nid];
		numa_fault_tmp[nid] = 0;
	}
	mma->numa_fault_tot = tot;
}

static int knumad_do_scan(void)
{
	struct mm_struct *mm;
	struct mm_autonuma *mm_autonuma;
	unsigned long address;
	struct vm_area_struct *vma;
	int progress = 0;

	mm = knumad_scan.mm;
	if (!mm) {
		if (unlikely(list_empty(&knumad_scan.mm_head)))
			return pages_to_scan;
		mm_autonuma = list_entry(knumad_scan.mm_head.next,
					 struct mm_autonuma, mm_node);
		mm = mm_autonuma->mm;
		knumad_scan.address = 0;
		knumad_scan.mm = mm;
		atomic_inc(&mm->mm_count);
		mm_autonuma->numa_fault_pass++;
	}
	address = knumad_scan.address;

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
			progress += knumad_scan_pmd(mm, vma, address);
			/* move to next address */
			address = (address + PMD_SIZE) & PMD_MASK;
			if (progress >= pages_to_scan)
				break;
		}
		end_addr = min(address, vma->vm_end);

		/*
		 * Flush the TLB for the mm to start the numa
		 * hinting minor page faults after we finish
		 * scanning this vma part.
		 */
		mmu_notifier_invalidate_range_start(vma->vm_mm, start_addr,
						    end_addr);
#if 0
#ifdef CONFIG_X86
		flush_tlb_all();
#else
		flush_tlb_range(vma, start_addr, end_addr);
#endif
#else
		flush_tlb_range(vma, start_addr, end_addr);
#endif
		mmu_notifier_invalidate_range_end(vma->vm_mm, start_addr,
						  end_addr);
	}
	up_read(&mm->mmap_sem); /* exit_mmap will destroy ptes after this */

	mutex_lock(&knumad_mm_mutex);
	VM_BUG_ON(knumad_scan.mm != mm);
	knumad_scan.address = address;
	/*
	 * Change the current mm if this mm is about to die, or if we
	 * scanned all vmas of this mm.
	 */
	if (knumad_test_exit(mm) || !vma) {
		mm_autonuma = mm->mm_autonuma;
		if (mm_autonuma->mm_node.next != &knumad_scan.mm_head) {
			mm_autonuma = list_entry(mm_autonuma->mm_node.next,
						 struct mm_autonuma, mm_node);
			knumad_scan.mm = mm_autonuma->mm;
			atomic_inc(&knumad_scan.mm->mm_count);
			knumad_scan.address = 0;
			knumad_scan.mm->mm_autonuma->numa_fault_pass++;
		} else
			knumad_scan.mm = NULL;

		if (knumad_test_exit(mm))
			list_del(&mm->mm_autonuma->mm_node);
		else
			mm_numa_fault_flush(mm);

		mmdrop(mm);
	}

	return progress;
}

static void wake_up_knuma_migrated(void)
{
	int nid;

	lru_add_drain();
	for_each_online_node(nid) {
		struct pglist_data *pgdat = NODE_DATA(nid);
		if (pgdat->autonuma_nr_migrate_pages &&
		    waitqueue_active(&pgdat->autonuma_knuma_migrated_wait))
			wake_up_interruptible(&pgdat->
					      autonuma_knuma_migrated_wait);
	}
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

	mutex_lock(&knumad_mm_mutex);

	for (;;) {
		if (unlikely(kthread_should_stop()))
			break;
		_progress = knumad_do_scan();
		progress += _progress;
		total_progress += _progress;
		mutex_unlock(&knumad_mm_mutex);

		if (unlikely(!knumad_scan.mm)) {
			autonuma_printk("knuma_scand %lu\n", total_progress);
			pages_scanned += total_progress;
			total_progress = 0;
			full_scans++;

			wait_event_freezable_timeout(knuma_scand_wait,
						     kthread_should_stop(),
						     msecs_to_jiffies(
						     scan_sleep_pass_millisecs));
			/* flush the last pending pages < pages_to_migrate */
			wake_up_knuma_migrated();
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

	mm = knumad_scan.mm;
	knumad_scan.mm = NULL;
	if (mm)
		list_del(&mm->mm_autonuma->mm_node);
	mutex_unlock(&knumad_mm_mutex);

	if (mm)
		mmdrop(mm);

	return 0;
}

static int isolate_migratepages(struct list_head *migratepages,
				struct pglist_data *pgdat)
{
	int nr = 0, nid;
	struct list_head *heads = pgdat->autonuma_migrate_head;

	/* FIXME: THP balancing, restart from last nid */
	for_each_online_node(nid) {
		struct zone *zone;
		struct page *page;
		cond_resched();
		VM_BUG_ON(numa_node_id() != pgdat->node_id);
		if (nid == pgdat->node_id) {
			VM_BUG_ON(!list_empty(&heads[nid]));
			continue;
		}
		if (list_empty(&heads[nid]))
			continue;
		/* some page wants to go to this pgdat */
		/*
		 * Take the lock with irqs disabled to avoid a lock
		 * inversion with the lru_lock which is taken before
		 * the autonuma_migrate_lock in split_huge_page, and
		 * that could be taken by interrupts after we obtained
		 * the autonuma_migrate_lock here, if we didn't disable
		 * irqs.
		 */
		autonuma_migrate_lock_irq(pgdat->node_id);
		if (list_empty(&heads[nid])) {
			autonuma_migrate_unlock_irq(pgdat->node_id);
			continue;
		}
		page = list_entry(heads[nid].prev,
				  struct page,
				  autonuma_migrate_node);
		if (unlikely(!get_page_unless_zero(page))) {
			/*
			 * Is getting freed and will remove self from the
			 * autonuma list shortly, skip it for now.
			 */
			list_del(&page->autonuma_migrate_node);
			list_add(&page->autonuma_migrate_node,
				 &heads[nid]);
			autonuma_migrate_unlock_irq(pgdat->node_id);
			autonuma_printk("autonuma migrate page is free\n");
			continue;
		}
		if (!PageLRU(page)) {
			autonuma_migrate_unlock_irq(pgdat->node_id);
			autonuma_printk("autonuma migrate page not in LRU\n");
			__autonuma_migrate_page_remove(page);
			put_page(page);
			continue;
		}
		autonuma_migrate_unlock_irq(pgdat->node_id);

		VM_BUG_ON(nid != page_to_nid(page));

		if (PageTransHuge(page))
			/* FIXME: remove split_huge_page */
			split_huge_page(page);

		__autonuma_migrate_page_remove(page);
		put_page(page);

		zone = page_zone(page);
		spin_lock_irq(&zone->lru_lock);
		if (!__isolate_lru_page(page, ISOLATE_ACTIVE|ISOLATE_INACTIVE,
					0)) {
			/* FIXME NR_ISOLATED */
			VM_BUG_ON(PageTransCompound(page));
			del_page_from_lru_list(zone, page, page_lru(page));
			inc_zone_state(zone, page_is_file_cache(page) ?
				       NR_ISOLATED_FILE : NR_ISOLATED_ANON);
			spin_unlock_irq(&zone->lru_lock);

			list_add(&page->lru, migratepages);
			nr += hpage_nr_pages(page);
		} else {
			/* FIXME: losing page, safest and simplest for now */
			spin_unlock_irq(&zone->lru_lock);
			autonuma_printk("autonuma migrate page lost\n");
		}

	}

	return nr;
}

static struct page *alloc_migrate_dst_page(struct page *page,
					   unsigned long data,
					   int **result)
{
	int nid = (int) data;
	struct page *newpage;
	newpage = alloc_pages_exact_node(nid,
					 GFP_HIGHUSER_MOVABLE | GFP_THISNODE,
					 0);
	if (newpage)
		newpage->autonuma_last_nid = page->autonuma_last_nid;
	return newpage;
}

static void knumad_do_migrate(struct pglist_data *pgdat)
{
	int nr_migrate_pages = 0;
	LIST_HEAD(migratepages);

	autonuma_printk("nr_migrate_pages %lu to node %d\n",
			pgdat->autonuma_nr_migrate_pages, pgdat->node_id);
	do {
		int isolated = 0;
		if (balance_pgdat(pgdat, nr_migrate_pages))
			isolated = isolate_migratepages(&migratepages, pgdat);
		/* FIXME: might need to check too many isolated */
		if (!isolated)
			break;
		nr_migrate_pages += isolated;
	} while (nr_migrate_pages < pages_to_migrate);

	if (nr_migrate_pages) {
		int err;
		autonuma_printk("migrate %d to node %d\n", nr_migrate_pages,
				pgdat->node_id);
		pages_migrated += nr_migrate_pages; /* FIXME: per node */
#if 1
		err = migrate_pages(&migratepages, alloc_migrate_dst_page,
				    pgdat->node_id, false, true);
#else
		err = 1;
#endif
		if (err)
			/* FIXME: requeue failed pages */
			putback_lru_pages(&migratepages);
	}
}

static int knuma_migrated(void *arg)
{
	struct pglist_data *pgdat = (struct pglist_data *)arg;
	int nid = pgdat->node_id;
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(nowakeup);

	set_freezable();

	for (;;) {
		if (unlikely(kthread_should_stop()))
			break;
		/* FIXME: scan the free levels of this node we may not
		 * be allowed to receive memory if the wmark of this
		 * pgdat are below high.  In the future also add
		 * not-interesting pages like not-accessed pages to
		 * pgdat->autonuma_migrate_head[pgdat->node_id]; so we
		 * can move our memory away to other nodes in order
		 * to satisfy the high-wmark described above (so migration
		 * can continue).
		 */
		knumad_do_migrate(pgdat);
		if (!pgdat->autonuma_nr_migrate_pages) {
			wait_event_freezable(
				pgdat->autonuma_knuma_migrated_wait,
				pgdat->autonuma_nr_migrate_pages ||
				kthread_should_stop());
			autonuma_printk("wake knuma_migrated %d\n", nid);
		} else
			wait_event_freezable_timeout(nowakeup,
						     kthread_should_stop(),
						     msecs_to_jiffies(
						     migrate_sleep_millisecs));
	}

	return 0;
}

void autonuma_enter(struct mm_struct *mm)
{
	if (autonuma_impossible())
		return;

	mutex_lock(&knumad_mm_mutex);
	list_add_tail(&mm->mm_autonuma->mm_node, &knumad_scan.mm_head);
	mutex_unlock(&knumad_mm_mutex);
}

void autonuma_exit(struct mm_struct *mm)
{
	bool serialize;

	if (autonuma_impossible())
		return;

	serialize = false;
	mutex_lock(&knumad_mm_mutex);
	if (knumad_scan.mm == mm)
		serialize = true;
	else
		list_del(&mm->mm_autonuma->mm_node);
	mutex_unlock(&knumad_mm_mutex);

	if (serialize) {
		down_write(&mm->mmap_sem);
		up_write(&mm->mmap_sem);
	}
}

static int start_knuma_scand(void)
{
	int err = 0;
	struct task_struct *knumad_thread;

	knumad_thread = kthread_run(knuma_scand, NULL, "knuma_scand");
	if (unlikely(IS_ERR(knumad_thread))) {
		autonuma_printk(KERN_ERR
				"knumad: kthread_run(knuma_scand) failed\n");
		err = PTR_ERR(knumad_thread);
	}
	return err;
}

static int start_knuma_migrated(void)
{
	int err = 0;
	struct task_struct *knumad_thread;
	int nid;

	for_each_online_node(nid) {
		knumad_thread = kthread_create_on_node(knuma_migrated,
						       NODE_DATA(nid),
						       nid,
						       "knuma_migrated%d",
						       nid);
		if (unlikely(IS_ERR(knumad_thread))) {
			autonuma_printk(KERN_ERR
					"knumad: "
					"kthread_run(knuma_migrated%d) "
					"failed\n", nid);
			err = PTR_ERR(knumad_thread);
		} else {
			autonuma_printk("cpumask %d %lx\n", nid,
					cpumask_of_node(nid)->bits[0]);
			kthread_bind_node(knumad_thread, nid);
			wake_up_process(knumad_thread);
		}
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
	return flag_show(kobj, attr, buf, AUTONUMA_FLAG);
}
static ssize_t enabled_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	ssize_t ret;

	ret = flag_store(kobj, attr, buf, count, AUTONUMA_FLAG);

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

SYSFS_ENTRY(debug, AUTONUMA_DEBUG_FLAG);
SYSFS_ENTRY(pmd, AUTONUMA_SCAN_PMD_FLAG);
SYSFS_ENTRY(working_set, AUTONUMA_SCAN_USE_WORKING_SET_FLAG);
SYSFS_ENTRY(defer, AUTONUMA_MIGRATE_DEFER_FLAG);
SYSFS_ENTRY(load_balance_strict, AUTONUMA_SCHED_LOAD_BALANCE_STRICT_FLAG);
SYSFS_ENTRY(clone_reset, AUTONUMA_SCHED_CLONE_RESET_FLAG);
SYSFS_ENTRY(fork_reset, AUTONUMA_SCHED_FORK_RESET_FLAG);

#undef SYSFS_ENTRY

enum {
	SYSFS_KNUMA_SCAND_SLEEP_ENTRY,
	SYSFS_KNUMA_SCAND_PAGES_ENTRY,
	SYSFS_KNUMA_MIGRATED_SLEEP_ENTRY,
	SYSFS_KNUMA_MIGRATED_PAGES_ENTRY,
};

#define SYSFS_ENTRY(NAME, SYSFS_TYPE)				\
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
	case SYSFS_KNUMA_SCAND_PAGES_ENTRY:			\
	case SYSFS_KNUMA_MIGRATED_PAGES_ENTRY:			\
		if (!val)					\
			return -EINVAL;				\
		break;						\
	}							\
								\
	NAME = val;						\
	switch (SYSFS_TYPE) {					\
	case SYSFS_KNUMA_SCAND_SLEEP_ENTRY:			\
		wake_up_interruptible(&knuma_scand_wait);	\
		break;						\
	case							\
		SYSFS_KNUMA_MIGRATED_SLEEP_ENTRY:		\
		wake_up_knuma_migrated();			\
		break;						\
	}							\
								\
	return count;						\
}								\
static struct kobj_attribute NAME ## _attr =			\
	__ATTR(NAME, 0644, NAME ## _show, NAME ## _store);

SYSFS_ENTRY(scan_sleep_millisecs, SYSFS_KNUMA_SCAND_SLEEP_ENTRY);
SYSFS_ENTRY(scan_sleep_pass_millisecs, SYSFS_KNUMA_SCAND_SLEEP_ENTRY);
SYSFS_ENTRY(pages_to_scan, SYSFS_KNUMA_SCAND_PAGES_ENTRY);

SYSFS_ENTRY(migrate_sleep_millisecs, SYSFS_KNUMA_MIGRATED_SLEEP_ENTRY);
SYSFS_ENTRY(pages_to_migrate, SYSFS_KNUMA_MIGRATED_PAGES_ENTRY);

#undef SYSFS_ENTRY

static struct attribute *autonuma_attr[] = {
	&enabled_attr.attr,
	&debug_attr.attr,
	NULL,
};
static struct attribute_group autonuma_attr_group = {
	.attrs = autonuma_attr,
};

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

static struct attribute *knuma_scand_attr[] = {
	&scan_sleep_millisecs_attr.attr,
	&scan_sleep_pass_millisecs_attr.attr,
	&pages_to_scan_attr.attr,
	&pages_scanned_attr.attr,
	&full_scans_attr.attr,
	&pmd_attr.attr,
	&working_set_attr.attr,
	NULL,
};
static struct attribute_group knuma_scand_attr_group = {
	.attrs = knuma_scand_attr,
	.name = "knuma_scand",
};

static struct attribute *knuma_migrated_attr[] = {
	&migrate_sleep_millisecs_attr.attr,
	&pages_to_migrate_attr.attr,
	&pages_migrated_attr.attr,
	&defer_attr.attr,
	NULL,
};
static struct attribute_group knuma_migrated_attr_group = {
	.attrs = knuma_migrated_attr,
	.name = "knuma_migrated",
};

static struct attribute *scheduler_attr[] = {
	&clone_reset_attr.attr,
	&fork_reset_attr.attr,
	&load_balance_strict_attr.attr,
	NULL,
};
static struct attribute_group scheduler_attr_group = {
	.attrs = scheduler_attr,
	.name = "scheduler",
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

	err = sysfs_create_group(*autonuma_kobj, &knuma_scand_attr_group);
	if (err) {
		printk(KERN_ERR
		       "autonuma: failed register knuma_scand group\n");
		goto remove_autonuma;
	}

	err = sysfs_create_group(*autonuma_kobj, &knuma_migrated_attr_group);
	if (err) {
		printk(KERN_ERR
		       "autonuma: failed register knuma_migrated group\n");
		goto remove_knuma_scand;
	}

	err = sysfs_create_group(*autonuma_kobj, &scheduler_attr_group);
	if (err) {
		printk(KERN_ERR
		       "autonuma: failed register scheduler group\n");
		goto remove_knuma_migrated;
	}

	return 0;

remove_knuma_migrated:
	sysfs_remove_group(*autonuma_kobj, &knuma_migrated_attr_group);
remove_knuma_scand:
	sysfs_remove_group(*autonuma_kobj, &knuma_scand_attr_group);
remove_autonuma:
	sysfs_remove_group(*autonuma_kobj, &autonuma_attr_group);
delete_obj:
	kobject_put(*autonuma_kobj);
	return err;
}

static void __init autonuma_exit_sysfs(struct kobject *autonuma_kobj)
{
	sysfs_remove_group(autonuma_kobj, &knuma_migrated_attr_group);
	sysfs_remove_group(autonuma_kobj, &knuma_scand_attr_group);
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
	if (!autonuma_impossible()) {
		printk("AutoNUMA permanently disabled\n");
		set_bit(AUTONUMA_IMPOSSIBLE, &autonuma_flags);
		BUG_ON(!autonuma_impossible());
	}
	return 1;
}
__setup("noautonuma", noautonuma_setup);

static int __init autonuma_init(void)
{
	int err;
	struct kobject *autonuma_kobj;

	VM_BUG_ON(num_possible_nodes() < 1);
	if (autonuma_impossible())
		return -EINVAL;

	err = autonuma_init_sysfs(&autonuma_kobj);
	if (err)
		return err;

	err = start_knuma_scand();
	if (err) {
		printk("failed to start knuma_scand\n");
		goto out;
	}
	err = start_knuma_migrated();
	if (err) {
		printk("failed to start knuma_migrated\n");
		goto out;
	}

	printk("AutoNUMA initialized successfully\n");
	return err;

out:
	autonuma_exit_sysfs(autonuma_kobj);
	return err;
}
module_init(autonuma_init)

static struct kmem_cache *sched_autonuma_cachep;

int alloc_sched_autonuma(struct task_struct *tsk, struct task_struct *orig,
			 int node)
{
	int err = 1;
	struct sched_autonuma *sched_autonuma;

	if (autonuma_impossible())
		goto no_numa;
	sched_autonuma = kmem_cache_alloc_node(sched_autonuma_cachep,
					       GFP_KERNEL, node);
	if (!sched_autonuma)
		goto out;
	if (autonuma_sched_clone_reset())
		sched_autonuma_reset(sched_autonuma);
	else {
		memcpy(sched_autonuma, orig->sched_autonuma,
		       sched_autonuma_size());
		BUG_ON(sched_autonuma->autonuma_stop_one_cpu);
	}
	tsk->sched_autonuma = sched_autonuma;
no_numa:
	err = 0;
out:
	return err;
}

void free_sched_autonuma(struct task_struct *tsk)
{
	if (autonuma_impossible()) {
		BUG_ON(tsk->sched_autonuma);
		return;
	}

	BUG_ON(!tsk->sched_autonuma);
	kmem_cache_free(sched_autonuma_cachep, tsk->sched_autonuma);
	tsk->sched_autonuma = NULL;
}

void __init sched_autonuma_init(void)
{
	struct sched_autonuma *sched_autonuma;

	BUG_ON(current != &init_task);

	if (autonuma_impossible())
		return;

	sched_autonuma_cachep =
		kmem_cache_create("sched_autonuma",
				  sched_autonuma_size(), 0,
				  SLAB_PANIC | SLAB_HWCACHE_ALIGN, NULL);

	sched_autonuma = kmem_cache_alloc_node(sched_autonuma_cachep,
					       GFP_KERNEL, numa_node_id());
	BUG_ON(!sched_autonuma);
	sched_autonuma_reset(sched_autonuma);
	BUG_ON(current->sched_autonuma);
	current->sched_autonuma = sched_autonuma;
}

static struct kmem_cache *mm_autonuma_cachep;

int alloc_mm_autonuma(struct mm_struct *mm)
{
	int err = 1;
	struct mm_autonuma *mm_autonuma;

	if (autonuma_impossible())
		goto no_numa;
	mm_autonuma = kmem_cache_alloc(mm_autonuma_cachep, GFP_KERNEL);
	if (!mm_autonuma)
		goto out;
	if (autonuma_sched_fork_reset() || !mm->mm_autonuma)
		mm_autonuma_reset(mm_autonuma);
	else
		memcpy(mm_autonuma, mm->mm_autonuma, mm_autonuma_size());
	mm->mm_autonuma = mm_autonuma;
	mm_autonuma->mm = mm;
no_numa:
	err = 0;
out:
	return err;
}

void free_mm_autonuma(struct mm_struct *mm)
{
	if (autonuma_impossible()) {
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

	if (autonuma_impossible())
		return;

	mm_autonuma_cachep =
		kmem_cache_create("mm_autonuma",
				  mm_autonuma_size(), 0,
				  SLAB_PANIC | SLAB_HWCACHE_ALIGN, NULL);
}
