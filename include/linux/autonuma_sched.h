#ifndef _LINUX_AUTONUMA_SCHED_H
#define _LINUX_AUTONUMA_SCHED_H

static bool inline task_autonuma_cpu(struct task_struct *p, int cpu)
{
#ifdef CONFIG_AUTONUMA
	int autonuma_node;
	struct task_autonuma *task_autonuma = p->task_autonuma;

	if (!task_autonuma)
		return true;

	autonuma_node = ACCESS_ONCE(task_autonuma->autonuma_node);
	if (autonuma_node < 0 || autonuma_node == cpu_to_node(cpu))
		return true;
	else
		return false;
#else
	return true;
#endif
}

static inline void sched_set_autonuma_need_balance(void)
{
#ifdef CONFIG_AUTONUMA
	struct task_autonuma *ta = current->task_autonuma;

	if (ta && current->mm)
		ta->autonuma_flags |= SCHED_AUTONUMA_FLAG_NEED_BALANCE;
#endif
}

#ifdef CONFIG_AUTONUMA
#include <linux/autonuma_flags.h>

extern void sched_autonuma_balance(void);
extern bool sched_autonuma_can_migrate_task(struct task_struct *p,
					    int numa, int dst_cpu,
					    enum cpu_idle_type idle);
#else /* CONFIG_AUTONUMA */
static inline void sched_autonuma_balance(void) {}
static inline bool sched_autonuma_can_migrate_task(struct task_struct *p,
						   int numa, int dst_cpu,
						   enum cpu_idle_type idle)
{
	return true;
}
#endif /* CONFIG_AUTONUMA */

#endif /* _LINUX_AUTONUMA_SCHED_H */
