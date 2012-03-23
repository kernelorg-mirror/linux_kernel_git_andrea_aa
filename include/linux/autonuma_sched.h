#ifndef _LINUX_AUTONUMA_SCHED_H
#define _LINUX_AUTONUMA_SCHED_H

#ifdef CONFIG_AUTONUMA
#include <linux/autonuma_flags.h>

extern void __sched_autonuma_balance(void);
extern bool sched_autonuma_can_migrate_task(struct task_struct *p,
					    int numa, int dst_cpu,
					    enum cpu_idle_type idle);
#else /* CONFIG_AUTONUMA */
static inline bool sched_autonuma_can_migrate_task(struct task_struct *p,
						   int numa, int dst_cpu,
						   enum cpu_idle_type idle)
{
	return true;
}
#endif /* CONFIG_AUTONUMA */

static bool inline task_autonuma_cpu(struct task_struct *p, int cpu)
{
#ifdef CONFIG_AUTONUMA
	int autonuma_nid;
	struct task_autonuma *task_autonuma = p->task_autonuma;

	if (!task_autonuma)
		return true;

	autonuma_nid = ACCESS_ONCE(task_autonuma->autonuma_nid);
	if (autonuma_nid < 0 || autonuma_nid == cpu_to_node(cpu))
		return true;
	else
		return false;
#else
	return true;
#endif
}

static inline void sched_autonuma_balance(void)
{
#ifdef CONFIG_AUTONUMA
	struct task_autonuma *ta = current->task_autonuma;

	if (ta && current->mm)
		__sched_autonuma_balance();
#endif
}

#endif /* _LINUX_AUTONUMA_SCHED_H */
