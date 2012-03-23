#ifndef _LINUX_AUTONUMA_SCHED_H
#define _LINUX_AUTONUMA_SCHED_H

#include <linux/autonuma_flags.h>

static bool inline task_autonuma_cpu(struct task_struct *p, int cpu)
{
#ifdef CONFIG_AUTONUMA
	int autonuma_node;
	struct sched_autonuma *sched_autonuma = p->sched_autonuma;

	if (!sched_autonuma)
		return true;

	autonuma_node = ACCESS_ONCE(sched_autonuma->autonuma_node);
	if (autonuma_node < 0 || autonuma_node == cpu_to_node(cpu))
		return true;
	else
		return false;
#else
	return true;
#endif
}

#ifdef CONFIG_AUTONUMA
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
