#ifndef _LINUX_AUTONUMA_TYPES_H
#define _LINUX_AUTONUMA_TYPES_H

#ifdef CONFIG_AUTONUMA

#include <linux/numa.h>

struct mm_autonuma {
	struct list_head mm_node;
	struct mm_struct *mm;
	unsigned long numa_fault_tot; /* reset from here */
	unsigned long numa_fault_pass;
	unsigned long numa_fault[0];
};

extern int alloc_mm_autonuma(struct mm_struct *mm);
extern void free_mm_autonuma(struct mm_struct *mm);
extern void __init mm_autonuma_init(void);

#define SCHED_AUTONUMA_FLAG_STOP_ONE_CPU	(1<<0)
#define SCHED_AUTONUMA_FLAG_NEED_BALANCE	(1<<1)

struct sched_autonuma {
	int autonuma_node;
	unsigned int autonuma_flags; /* zeroed from here */
	unsigned long numa_fault_pass;
	unsigned long numa_fault_tot;
	unsigned long numa_fault[0];
};

struct page_autonuma {
	/*
	 * FIXME: move to pgdat section along with the memcg and allocate
	 * at runtime only in presence of a numa system.
	 */
	/*
	 * To modify autonuma_last_nid lockless the architecture,
	 * needs SMP atomic granularity < sizeof(long), not all archs
	 * have that, notably some alpha. Archs without that requires
	 * autonuma_last_nid to be a long.
	 */
#if BITS_PER_LONG > 32
	int autonuma_migrate_nid;
	int autonuma_last_nid;
#else
#if MAX_NUMNODES >= 32768
#error "too many nodes"
#endif
	/* FIXME: remember to check the updates are atomic */
	short autonuma_migrate_nid;
	short autonuma_last_nid;
#endif
	struct list_head autonuma_migrate_node;

	/*
	 * To find the page starting from the autonuma_migrate_node we
	 * need a backlink.
	 */
	struct page *page;
};

extern int alloc_sched_autonuma(struct task_struct *tsk,
				struct task_struct *orig,
				int node);
extern void __init sched_autonuma_init(void);
extern void free_sched_autonuma(struct task_struct *tsk);

#else /* CONFIG_AUTONUMA */

static inline int alloc_mm_autonuma(struct mm_struct *mm)
{
	return 0;
}
static inline void free_mm_autonuma(struct mm_struct *mm) {}
static inline void mm_autonuma_init(void) {}

static inline int alloc_sched_autonuma(struct task_struct *tsk,
				       struct task_struct *orig,
				       int node)
{
	return 0;
}
static inline void sched_autonuma_init(void) {}
static inline void free_sched_autonuma(struct task_struct *tsk) {}

#endif /* CONFIG_AUTONUMA */

#endif /* _LINUX_AUTONUMA_TYPES_H */
