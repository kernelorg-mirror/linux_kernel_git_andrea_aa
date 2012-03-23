/*
 *  Copyright (C) 2012  Red Hat, Inc.
 *
 *  This work is licensed under the terms of the GNU GPL, version 2. See
 *  the COPYING file in the top-level directory.
 */

#include <linux/sched.h>
#include <linux/autonuma_sched.h>
#include <asm/tlb.h>

#include "sched.h"

#define AUTONUMA_BALANCE_SCALE 1000

enum {
	W_TYPE_THREAD,
	W_TYPE_PROCESS,
};

/*
 * This function is responsible for deciding which is the best CPU
 * each process should be running on according to the NUMA
 * affinity. To do that it evaluates all CPUs and checks if there's
 * any remote CPU where the current process has more NUMA affinity
 * than with the current CPU, and where the process running on the
 * remote CPU has less NUMA affinity than the current process to run
 * on the remote CPU. Ideally this should be expanded to take all
 * runnable processes into account but this is a good
 * approximation. When we compare the NUMA affinity between the
 * current and remote CPU we use the per-thread information if the
 * remote CPU runs a thread of the same process that the current task
 * belongs to, or the per-process information if the remote CPU runs a
 * different process than the current one. If the remote CPU runs the
 * idle task we require both the per-thread and per-process
 * information to have more affinity with the remote CPU than with the
 * current CPU for a migration to happen.
 *
 * This has O(N) complexity but N isn't the number of running
 * processes, but the number of CPUs, so if you assume a constant
 * number of CPUs (capped at NR_CPUS) it is O(1). O(1) misleading math
 * aside, the number of cachelines touched with thousands of CPU might
 * make it measurable. Calling this at every schedule may also be
 * overkill and it may be enough to call it with a frequency similar
 * to the load balancing, but by doing so we're also verifying the
 * algorithm is a converging one in all workloads if performance is
 * improved and there's no frequent CPU migration, so it's good in the
 * short term for stressing the algorithm.
 */
void sched_autonuma_balance(void)
{
	int cpu, nid, selected_cpu, selected_nid;
	int cpu_nid = numa_node_id();
	int this_cpu = smp_processor_id();
	unsigned long p_w, p_t, m_w, m_t, p_w_max, m_w_max;
	unsigned long weight_delta_max, weight;
	long s_w_nid = -1, s_w_cpu_nid = -1, s_w_other = -1;
	int s_w_type = -1;
	struct cpumask *allowed;
	struct migration_arg arg;
	struct task_struct *p = current;
	struct sched_autonuma *sched_autonuma = p->sched_autonuma;

	/* per-cpu statically allocated in runqueues */
	long *weight_current;
	long *weight_current_mm;

	if (!sched_autonuma || sched_autonuma->autonuma_stop_one_cpu || !p->mm)
		return;

	if (!autonuma_enabled()) {
		if (sched_autonuma->autonuma_node != -1)
			sched_autonuma->autonuma_node = -1;
		return;
	}

	allowed = tsk_cpus_allowed(p);

	m_t = ACCESS_ONCE(p->mm->mm_autonuma->numa_fault_tot);
	p_t = sched_autonuma->numa_fault_tot;
	/*
	 * If a process still misses the per-thread or per-process
	 * information skip it.
	 */
	if (!m_t || !p_t)
		return;

	weight_current = cpu_rq(this_cpu)->weight_current;
	weight_current_mm = cpu_rq(this_cpu)->weight_current_mm;

	p_w_max = m_w_max = 0;
	selected_nid = -1;
	for_each_online_node(nid) {
		int hits = 0;
		m_w = ACCESS_ONCE(p->mm->mm_autonuma->numa_fault[nid]);
		p_w = sched_autonuma->numa_fault[nid];
		if (m_w > m_t)
			m_t = m_w;
		weight_current_mm[nid] = m_w*AUTONUMA_BALANCE_SCALE/m_t;
		if (p_w > p_t)
			p_t = p_w;
		weight_current[nid] = p_w*AUTONUMA_BALANCE_SCALE/p_t;
		if (weight_current_mm[nid] > m_w_max) {
			m_w_max = weight_current_mm[nid];
			hits++;
		}
		if (weight_current[nid] > p_w_max) {
			p_w_max = weight_current[nid];
			hits++;
		}
		if (hits == 2)
			selected_nid = nid;
	}
	if (selected_nid == cpu_nid)
		return;

	selected_cpu = this_cpu;
	selected_nid = cpu_nid;
	weight = weight_delta_max = 0;

	for_each_online_node(nid) {
		if (nid == cpu_nid)
			continue;
		for_each_cpu_and(cpu, cpumask_of_node(nid), allowed) {
			long w_nid, w_cpu_nid, w_other;
			int w_type;
			struct mm_struct *mm;
			struct rq *rq = cpu_rq(cpu);
			if (!cpu_online(cpu))
				continue;

			if (idle_cpu(cpu))
				/*
				 * Offload the while IDLE balancing
				 * and physical / logical imbalances
				 * to CFS.
				 */
				continue;

			mm = rq->curr->mm;
			if (!mm)
				continue;
			raw_spin_lock_irq(&rq->lock);
			/* recheck after implicit barrier() */
			mm = rq->curr->mm;
			if (!mm) {
				raw_spin_unlock_irq(&rq->lock);
				continue;
			}
			m_t = ACCESS_ONCE(mm->mm_autonuma->numa_fault_tot);
			p_t = rq->curr->sched_autonuma->numa_fault_tot;
			if (!m_t || !p_t) {
				raw_spin_unlock_irq(&rq->lock);
				continue;
			}
			m_w = ACCESS_ONCE(mm->mm_autonuma->numa_fault[nid]);
			p_w = rq->curr->sched_autonuma->numa_fault[nid];
			raw_spin_unlock_irq(&rq->lock);
			if (mm == p->mm) {
				if (p_w > p_t)
					p_t = p_w;
				w_other = p_w*AUTONUMA_BALANCE_SCALE/p_t;
				w_nid = weight_current[nid];
				w_cpu_nid = weight_current[cpu_nid];
				w_type = W_TYPE_THREAD;
			} else {
				if (m_w > m_t)
					m_t = m_w;
				w_other = m_w*AUTONUMA_BALANCE_SCALE/m_t;
				w_nid = weight_current_mm[nid];
				w_cpu_nid = weight_current_mm[cpu_nid];
				w_type = W_TYPE_PROCESS;
			}

			if (w_nid > w_other && w_nid > w_cpu_nid) {
				weight = w_nid - w_other + w_nid - w_cpu_nid;

				if (weight > weight_delta_max) {
					weight_delta_max = weight;
					selected_cpu = cpu;
					selected_nid = nid;

					s_w_other = w_other;
					s_w_nid = w_nid;
					s_w_cpu_nid = w_cpu_nid;
					s_w_type = w_type;
				}
			}
		}
	}

	if (sched_autonuma->autonuma_node != selected_nid)
		sched_autonuma->autonuma_node = selected_nid;
	if (selected_cpu != this_cpu) {
		if (autonuma_debug()) {
			char *w_type_str = NULL;
			switch (s_w_type) {
			case W_TYPE_THREAD:
				w_type_str = "thread";
				break;
			case W_TYPE_PROCESS:
				w_type_str = "process";
				break;
			}
			printk("%p %d - %dto%d - %dto%d - %ld %ld %ld - %s\n",
			       p->mm, p->pid, cpu_nid, selected_nid,
			       this_cpu, selected_cpu,
			       s_w_other, s_w_nid, s_w_cpu_nid,
			       w_type_str);
		}
		BUG_ON(cpu_nid == selected_nid);
		goto found;
	}

	return;

found:
	arg = (struct migration_arg) { p, selected_cpu };
	/* Need help from migration thread: drop lock and wait. */
	sched_autonuma->autonuma_stop_one_cpu = true;
	sched_preempt_enable_no_resched();
	stop_one_cpu(this_cpu, migration_cpu_stop, &arg);
	preempt_disable();
	sched_autonuma->autonuma_stop_one_cpu = false;
	tlb_migrate_finish(p->mm);
}

bool sched_autonuma_can_migrate_task(struct task_struct *p,
				     int numa, int dst_cpu,
				     enum cpu_idle_type idle)
{
	if (!task_autonuma_cpu(p, dst_cpu)) {
		if (numa)
			return false;
		if (autonuma_sched_load_balance_strict() &&
		    idle != CPU_NEWLY_IDLE && idle != CPU_IDLE)
			return false;
	}
	return true;
}

void sched_autonuma_dump_mm(void)
{
	int nid, cpu;
	cpumask_var_t x;

	if (!alloc_cpumask_var(&x, GFP_KERNEL))
		return;
	cpumask_setall(x);
	for_each_online_node(nid) {
		for_each_cpu(cpu, cpumask_of_node(nid)) {
			struct rq *rq = cpu_rq(cpu);
			struct mm_struct *mm = rq->curr->mm;
			int nr = 0, cpux;
			if (!cpumask_test_cpu(cpu, x))
				continue;
			for_each_cpu(cpux, cpumask_of_node(nid)) {
				struct rq *rqx = cpu_rq(cpux);
				if (rqx->curr->mm == mm) {
					nr++;
					cpumask_clear_cpu(cpux, x);
				}
			}
			printk("nid %d mm %p nr %d\n", nid, mm, nr);
		}
	}
	free_cpumask_var(x);
}
