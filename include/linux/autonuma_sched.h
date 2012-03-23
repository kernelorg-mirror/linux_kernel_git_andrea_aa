#ifndef _LINUX_AUTONUMA_SCHED_H
#define _LINUX_AUTONUMA_SCHED_H

#ifdef CONFIG_AUTONUMA
#include <linux/autonuma_flags.h>

extern void __sched_autonuma_balance(void);
extern bool sched_autonuma_can_migrate_task(struct task_struct *p,
					    int numa, int dst_cpu,
					    enum cpu_idle_type idle);

static bool inline task_autonuma_cpu(struct task_struct *p, int cpu)
{
	int task_selected_nid;
	struct task_autonuma *task_autonuma = p->task_autonuma;

	if (!task_autonuma)
		return true;

	task_selected_nid = ACCESS_ONCE(task_autonuma->task_selected_nid);
	if (task_selected_nid < 0 || task_selected_nid == cpu_to_node(cpu))
		return true;
	else
		return false;
}

static inline void sched_autonuma_balance(void)
{
	struct task_autonuma *ta = current->task_autonuma;

	if (ta && current->mm)
		__sched_autonuma_balance();
}
#else /* CONFIG_AUTONUMA */
static inline bool sched_autonuma_can_migrate_task(struct task_struct *p,
						   int numa, int dst_cpu,
						   enum cpu_idle_type idle)
{
	return true;
}

static bool inline task_autonuma_cpu(struct task_struct *p, int cpu)
{
	return true;
}

static inline void sched_autonuma_balance(void) {}
#endif /* CONFIG_AUTONUMA */

#endif /* _LINUX_AUTONUMA_SCHED_H */
