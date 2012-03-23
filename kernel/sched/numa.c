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
 * runnable processes into account but this is a good enough
 * approximation. When we compare the NUMA affinity between the
 * current and remote CPU we use the per-thread information if the
 * remote CPU runs a thread of the same process that the current task
 * belongs to, or the per-process information if the remote CPU runs a
 * different process than the current one.
 */
void sched_autonuma_balance(void)
{
	int cpu, nid, selected_cpu, selected_nid, selected_nid_mm;
	int cpu_nid = numa_node_id();
	int this_cpu = smp_processor_id();
	unsigned long t_w, t_t, m_w, m_t, t_w_max, m_w_max;
	unsigned long weight_delta_max, weight;
	long s_w_nid = -1, s_w_cpu_nid = -1, s_w_other = -1;
	int s_w_type = -1;
	struct cpumask *allowed;
	struct migration_arg arg;
	struct task_struct *p = current;
	struct task_autonuma *task_autonuma = p->task_autonuma;

	/* per-cpu statically allocated in runqueues */
	long *task_numa_weight;
	long *mm_numa_weight;

	if (!task_autonuma || !p->mm)
		return;

	if (!(task_autonuma->autonuma_flags &
	      SCHED_AUTONUMA_FLAG_NEED_BALANCE))
		return;
	else
		task_autonuma->autonuma_flags &=
			~SCHED_AUTONUMA_FLAG_NEED_BALANCE;

	if (task_autonuma->autonuma_flags & SCHED_AUTONUMA_FLAG_STOP_ONE_CPU)
		return;

	if (!autonuma_enabled()) {
		if (task_autonuma->autonuma_node != -1)
			task_autonuma->autonuma_node = -1;
		return;
	}

	allowed = tsk_cpus_allowed(p);

	m_t = ACCESS_ONCE(p->mm->mm_autonuma->mm_numa_fault_tot);
	t_t = task_autonuma->task_numa_fault_tot;
	/*
	 * If a process still misses the per-thread or per-process
	 * information skip it.
	 */
	if (!m_t || !t_t)
		return;

	task_numa_weight = cpu_rq(this_cpu)->task_numa_weight;
	mm_numa_weight = cpu_rq(this_cpu)->mm_numa_weight;

	/*
	 * See if we already converged to skip the more expensive loop
	 * below, if we can already predict here with only CPU local
	 * information, that it would selected the current cpu_nid.
	 */
	t_w_max = m_w_max = 0;
	selected_nid = selected_nid_mm = -1;
	for_each_online_node(nid) {
		m_w = ACCESS_ONCE(p->mm->mm_autonuma->mm_numa_fault[nid]);
		t_w = task_autonuma->task_numa_fault[nid];
		if (m_w > m_t)
			m_t = m_w;
		mm_numa_weight[nid] = m_w*AUTONUMA_BALANCE_SCALE/m_t;
		if (t_w > t_t)
			t_t = t_w;
		task_numa_weight[nid] = t_w*AUTONUMA_BALANCE_SCALE/t_t;
		if (mm_numa_weight[nid] > m_w_max) {
			m_w_max = mm_numa_weight[nid];
			selected_nid_mm = nid;
		}
		if (task_numa_weight[nid] > t_w_max) {
			t_w_max = task_numa_weight[nid];
			selected_nid = nid;
		}
	}
	if (selected_nid == cpu_nid && selected_nid_mm == selected_nid) {
		if (task_autonuma->autonuma_node != selected_nid)
			task_autonuma->autonuma_node = selected_nid;
		return;
	}

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
			m_t = ACCESS_ONCE(mm->mm_autonuma->mm_numa_fault_tot);
			t_t = rq->curr->task_autonuma->task_numa_fault_tot;
			if (!m_t || !t_t) {
				raw_spin_unlock_irq(&rq->lock);
				continue;
			}
			m_w = ACCESS_ONCE(mm->mm_autonuma->mm_numa_fault[nid]);
			t_w = rq->curr->task_autonuma->task_numa_fault[nid];
			raw_spin_unlock_irq(&rq->lock);
			if (mm == p->mm) {
				if (t_w > t_t)
					t_t = t_w;
				w_other = t_w*AUTONUMA_BALANCE_SCALE/t_t;
				w_nid = task_numa_weight[nid];
				w_cpu_nid = task_numa_weight[cpu_nid];
				w_type = W_TYPE_THREAD;
			} else {
				if (m_w > m_t)
					m_t = m_w;
				w_other = m_w*AUTONUMA_BALANCE_SCALE/m_t;
				w_nid = mm_numa_weight[nid];
				w_cpu_nid = mm_numa_weight[cpu_nid];
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

	if (task_autonuma->autonuma_node != selected_nid)
		task_autonuma->autonuma_node = selected_nid;
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
	task_autonuma->autonuma_flags |= SCHED_AUTONUMA_FLAG_STOP_ONE_CPU;
	sched_preempt_enable_no_resched();
	stop_one_cpu(this_cpu, migration_cpu_stop, &arg);
	preempt_disable();
	task_autonuma->autonuma_flags &= ~SCHED_AUTONUMA_FLAG_STOP_ONE_CPU;
	tlb_migrate_finish(p->mm);
}

/*
 * This is called by CFS can_migrate_task() to prioritize the
 * selection of AutoNUMA affine tasks (according to the autonuma_node)
 * during the CFS load balance, active balance, etc...
 *
 * This is first called with numa == true to skip not AutoNUMA affine
 * tasks. If this is later called with a numa == false parameter, it
 * means a first pass of CFS load balancing wasn't satisfied by an
 * AutoNUMA affine task and so we can decide to fallback to allowing
 * migration of not affine tasks.
 *
 * If load_balance_strict is enabled, AutoNUMA will only allow
 * migration of tasks for idle balancing purposes (the idle balancing
 * of CFS is never altered by AutoNUMA). In the not strict mode the
 * load balancing is not altered and the AutoNUMA affinity is
 * disregarded in favor of higher fairness
 *
 * The load_balance_strict mode (tunable through sysfs), if enabled,
 * tends to partition the system and in turn it may reduce the
 * scheduler fariness across different NUMA nodes but it shall deliver
 * higher global performance.
 */
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
