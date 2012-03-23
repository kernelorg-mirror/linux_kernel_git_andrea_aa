#ifndef _LINUX_AUTONUMA_H
#define _LINUX_AUTONUMA_H

#ifdef CONFIG_AUTONUMA

#include <linux/autonuma_flags.h>

extern void autonuma_enter(struct mm_struct *mm);
extern void autonuma_exit(struct mm_struct *mm);
extern void __autonuma_migrate_page_remove(struct page *page);
extern void autonuma_migrate_split_huge_page(struct page *page,
					     struct page *page_tail);
extern void autonuma_setup_new_exec(struct task_struct *p);

static inline void autonuma_migrate_page_remove(struct page *page)
{
	if (ACCESS_ONCE(page->autonuma_migrate_nid) >= 0)
		__autonuma_migrate_page_remove(page);
}

#define autonuma_printk(format, args...) \
	if (autonuma_debug()) printk(format, ##args)

#else /* CONFIG_AUTONUMA */

static inline void autonuma_enter(struct mm_struct *mm) {}
static inline void autonuma_exit(struct mm_struct *mm) {}
static inline void autonuma_migrate_page_remove(struct page *page) {}
static inline void autonuma_migrate_split_huge_page(struct page *page,
						    struct page *page_tail) {}
static inline void autonuma_setup_new_exec(struct task_struct *p) {}

#endif /* CONFIG_AUTONUMA */

extern int pte_numa_fixup(struct mm_struct *mm, struct vm_area_struct *vma,
			  unsigned long addr, pte_t pte, pte_t *ptep,
			  pmd_t *pmd);
extern int pmd_numa_fixup(struct mm_struct *mm, unsigned long addr,
			  pmd_t *pmd);
extern void numa_hinting_fault(struct page *page, int numpages);

#endif /* _LINUX_AUTONUMA_H */
