#ifndef _LINUX_AUTONUMA_H
#define _LINUX_AUTONUMA_H

#include <linux/autonuma_flags.h>

#ifdef CONFIG_AUTONUMA

extern void autonuma_enter(struct mm_struct *mm);
extern void autonuma_exit(struct mm_struct *mm);
extern void autonuma_migrate_split_huge_page(struct page *page,
					     struct page *page_tail);
extern void autonuma_setup_new_exec(struct task_struct *p);
extern struct page_autonuma *lookup_page_autonuma(struct page *page);

static inline void autonuma_free_page(struct page *page)
{
	if (autonuma_possible())
		lookup_page_autonuma(page)->autonuma_last_nid = -1;
}

static inline int autonuma_check_new_page(struct page *page)
{
	struct page_autonuma *page_autonuma;
	int ret = 0;
	if (autonuma_possible()) {
		page_autonuma = lookup_page_autonuma(page);
		if (unlikely(page_autonuma->autonuma_last_nid != -1)) {
			ret = 1;
			WARN_ON(1);
		}
	}
	return ret;
}

#define autonuma_printk(format, args...) \
	if (autonuma_debug()) printk(format, ##args)

#else /* CONFIG_AUTONUMA */

static inline void autonuma_enter(struct mm_struct *mm) {}
static inline void autonuma_exit(struct mm_struct *mm) {}
static inline void autonuma_migrate_split_huge_page(struct page *page,
						    struct page *page_tail) {}
static inline void autonuma_setup_new_exec(struct task_struct *p) {}
static inline void autonuma_free_page(struct page *page) {}
static inline int autonuma_check_new_page(struct page *page) { return 0; }

#endif /* CONFIG_AUTONUMA */

extern int pte_numa_fixup(struct mm_struct *mm, struct vm_area_struct *vma,
			  unsigned long addr, pte_t pte, pte_t *ptep,
			  pmd_t *pmd);
extern int pmd_numa_fixup(struct mm_struct *mm, unsigned long addr,
			  pmd_t *pmd);
extern bool numa_hinting_fault(struct page *page, int numpages);

#endif /* _LINUX_AUTONUMA_H */
