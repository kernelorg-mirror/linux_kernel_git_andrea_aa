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

struct sched_autonuma {
	int autonuma_node;
	bool autonuma_stop_one_cpu; /* reset from here */
	unsigned long numa_fault_pass;
	unsigned long numa_fault_tot;
	unsigned long numa_fault[0];
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
