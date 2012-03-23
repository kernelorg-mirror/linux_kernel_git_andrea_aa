#ifndef _LINUX_AUTONUMA_H
#define _LINUX_AUTONUMA_H

#include <linux/autonuma_flags.h>

#ifdef CONFIG_AUTONUMA

extern void autonuma_enter(struct mm_struct *mm);
extern void autonuma_exit(struct mm_struct *mm);
extern void autonuma_migrate_split_huge_page(struct page *page,
					     struct page *page_tail);
extern void autonuma_setup_new_exec(struct task_struct *p);

#define autonuma_printk(format, args...) \
	if (autonuma_debug()) printk(format, ##args)

#else /* CONFIG_AUTONUMA */

static inline void autonuma_enter(struct mm_struct *mm) {}
static inline void autonuma_exit(struct mm_struct *mm) {}
static inline void autonuma_migrate_split_huge_page(struct page *page,
						    struct page *page_tail) {}
static inline void autonuma_setup_new_exec(struct task_struct *p) {}

#endif /* CONFIG_AUTONUMA */

extern int pte_numa_fixup(struct mm_struct *mm, struct vm_area_struct *vma,
			  unsigned long addr, pte_t pte, pte_t *ptep,
			  pmd_t *pmd);
extern int pmd_numa_fixup(struct mm_struct *mm, unsigned long addr,
			  pmd_t *pmd);
extern bool numa_hinting_fault(struct page *page, int numpages);

#endif /* _LINUX_AUTONUMA_H */
