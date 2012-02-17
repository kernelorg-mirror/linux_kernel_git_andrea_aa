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

//#define AUTONUMA_BALANCE_BLIND
#ifdef AUTONUMA_BALANCE_BLIND
static int autonuma_balance_blind(struct task_struct *p, int this_cpu,
				  int cpu_nid, struct cpumask *allowed,
				  int *selected_nid_p)
{
	int nid, cpu, nr_mm, nr_mm_max, selected_nid;
	struct mm_struct *mm;
	DECLARE_BITMAP(nodes, MAX_NUMNODES);

#if 1
	if (p->autonuma_node >= 0)
		return -1;
#endif

	bitmap_zero(nodes, MAX_NUMNODES);
	__set_bit(cpu_nid, nodes);
	mm = p->mm;

	for (;;) {
		selected_nid = cpu_nid;

		nr_mm_max = 0;
		for_each_cpu_and(cpu, cpumask_of_node(nid), allowed) {
			struct rq *rq = cpu_rq(cpu);
			if (rq->curr->mm == mm)
				nr_mm_max++;
		}
		for_each_online_node(nid) {
			if (test_bit(nid, nodes))
				continue;
			nr_mm = 0;
			for_each_cpu_and(cpu, cpumask_of_node(nid), allowed) {
				struct rq *rq = cpu_rq(cpu);
				if (rq->curr->mm == mm)
					nr_mm++;
			}
			if (nr_mm > nr_mm_max) {
				nr_mm_max = nr_mm;
				selected_nid = nid;
			}
		}

		if (selected_nid == cpu_nid) {
			*selected_nid_p = selected_nid;
			return this_cpu;
		}

		for_each_cpu_and(cpu, cpumask_of_node(selected_nid), allowed) {
			struct rq *rq = cpu_rq(cpu);
			if (idle_cpu(cpu) &&
			    rq->avg_idle > sysctl_sched_migration_cost) {
				*selected_nid_p = selected_nid;
				return cpu;
			}
		}
		__set_bit(selected_nid, nodes);
	}
}
#endif /* AUTONUMA_BALANCE_BLIND */

#define AUTONUMA_BALANCE_SCALE 1000

/*
 * node
 * 90 10 task
 * 95 5  current
 * 75 20 task
 * 0  0  idle
 */
void sched_autonuma_balance(void)
{
	int cpu, nid, selected_cpu, selected_nid;
	int cpu_nid = numa_node_id();
	int this_cpu = smp_processor_id();
	long weight_others[NR_CPUS];
	long weight_current[MAX_NUMNODES];
	long weight_current_mm[MAX_NUMNODES];
	unsigned long p_w, p_t, m_w, m_t;
	unsigned long weight_delta_max, weight;
	struct cpumask *allowed;
	struct migration_arg arg;
	struct task_struct *p = current;
	struct sched_autonuma *sched_autonuma = p->sched_autonuma;
	DECLARE_BITMAP(mm_mask, MAX_NUMNODES);

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
	if (!m_t || !p_t) {
#ifdef AUTONUMA_BALANCE_BLIND
		selected_cpu = autonuma_balance_blind(p, this_cpu, cpu_nid,
						      allowed, &selected_nid);
		if (selected_cpu < 0)
			return;
		goto selected;
#else
		return;
#endif
	}

	for_each_online_node(nid) {
		m_w = ACCESS_ONCE(p->mm->mm_autonuma->numa_fault[nid]);
		p_w = sched_autonuma->numa_fault[nid];
		if (m_w > m_t)
			m_t = m_w;
		//weight_current[nid] = m_w*AUTONUMA_BALANCE_SCALE/m_t;
		//weight_current[nid] = m_w;
		weight_current_mm[nid] = m_w*AUTONUMA_BALANCE_SCALE/m_t;
		if (p_w > p_t)
			p_t = p_w;
		//weight_current[nid] += p_w*AUTONUMA_BALANCE_SCALE/p_t;
		weight_current[nid] = p_w*AUTONUMA_BALANCE_SCALE/p_t;
	}

	bitmap_zero(mm_mask, MAX_NUMNODES);
	for_each_online_node(nid) {
		if (nid == cpu_nid)
			continue;
		for_each_cpu_and(cpu, cpumask_of_node(nid), allowed) {
			struct mm_struct *mm;
			struct rq *rq = cpu_rq(cpu);
			//weight_others[cpu] = AUTONUMA_BALANCE_SCALE*2+1;
			weight_others[cpu] = LONG_MAX;
#if 1
			if (idle_cpu(cpu) &&
			    rq->avg_idle > sysctl_sched_migration_cost) {
				if (weight_current[nid] >
				    weight_current[cpu_nid] &&
				    weight_current_mm[nid] >
				    weight_current_mm[cpu_nid])
					weight_others[cpu] = -1;
				continue;
			}
#endif
			mm = rq->curr->mm;
			if (!mm)
				continue;
			raw_spin_lock_irq(&rq->lock);
			/* recheck after implicit barrier() */
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
			if (m_w > m_t)
				m_t = m_w;
			//weight_others[cpu] = m_w*AUTONUMA_BALANCE_SCALE/m_t;
			//weight_others[cpu] = m_w;
			weight_others[cpu] = m_w*AUTONUMA_BALANCE_SCALE/m_t;
			if (p_w > p_t)
				p_t = p_w;
			//weight_others[cpu] += p_w*
			//	AUTONUMA_BALANCE_SCALE/p_t;
			//weight_others[cpu] = p_w*
			//	AUTONUMA_BALANCE_SCALE/p_t;
			if (mm == p->mm) {
				__set_bit(cpu, mm_mask);
				weight_others[cpu] = p_w*
					AUTONUMA_BALANCE_SCALE/p_t;
			}
		}
	}

	selected_cpu = this_cpu;
	selected_nid = cpu_nid;
	weight_delta_max = 0;

	for_each_online_node(nid) {
		if (nid == cpu_nid)
			continue;
		for_each_cpu_and(cpu, cpumask_of_node(nid), allowed) {
			long w_nid, w_cpu_nid;
			if (test_bit(cpu, mm_mask)) {
				w_nid = weight_current[nid];
				w_cpu_nid = weight_current[cpu_nid];
			} else {
				w_nid = weight_current_mm[nid];
				w_cpu_nid = weight_current_mm[cpu_nid];
			}
			if (w_nid > weight_others[cpu] &&
			    w_nid > w_cpu_nid) {
				weight = w_nid -
					weight_others[cpu] +
					w_nid -
					w_cpu_nid;
				if (weight > weight_delta_max) {
					weight_delta_max = weight;
					selected_cpu = cpu;
					selected_nid = nid;
				}
			}
		}
	}

#ifdef AUTONUMA_BALANCE_BLIND
selected:
#endif
	if (sched_autonuma->autonuma_node != selected_nid)
		sched_autonuma->autonuma_node = selected_nid;
	if (selected_cpu != this_cpu) {
#if 0
		struct rq *rq = cpu_rq(selected_cpu);
		raw_spin_lock_irq(&rq->lock);
		if (rq->curr->autonuma_node == selected_nid)
			rq->curr->autonuma_node = cpu_nid;
		raw_spin_unlock_irq(&rq->lock);
#endif
		if (autonuma_debug())
			printk("%p %d - %dto%d - %dto%d - %ld %ld %ld - %s\n",
			       p->mm, p->pid, cpu_nid, selected_nid,
			       this_cpu, selected_cpu,
			       weight_others[selected_cpu],
			       test_bit(selected_cpu, mm_mask) ?
			       weight_current[selected_nid] :
			       weight_current_mm[selected_nid],
			       test_bit(selected_cpu, mm_mask) ?
			       weight_current[cpu_nid] :
			       weight_current_mm[cpu_nid],
			       test_bit(selected_cpu, mm_mask) ?
			       "thread" : "process");
		BUG_ON(cpu_nid == selected_nid);
		goto found;
	}

	return;

found:
	arg = (struct migration_arg) { p, selected_cpu };
	/* Need help from migration thread: drop lock and wait. */
	sched_autonuma->autonuma_stop_one_cpu = true;
	preempt_enable_no_resched();
	stop_one_cpu(this_cpu, migration_cpu_stop, &arg);
	preempt_disable();
	sched_autonuma->autonuma_stop_one_cpu = false;
	tlb_migrate_finish(p->mm);
}

bool sched_autonuma_can_migrate_task(struct task_struct *p, int this_cpu,
				     enum cpu_idle_type idle,
				     struct cpumask *allowed)
{
	if (!task_autonuma_cpu(p, this_cpu)) {
		int cpu;
		int autonuma_node;

		autonuma_node = ACCESS_ONCE(p->sched_autonuma->autonuma_node);
		if (autonuma_load_balance_strict() &&
		    idle != CPU_NEWLY_IDLE && idle != CPU_IDLE)
			return false;
		if (idle == CPU_NUMA)
			return false;
		for_each_cpu_and(cpu, cpumask_of_node(autonuma_node),
				 allowed) {
			struct rq *rq = cpu_rq(cpu);
			int _autonuma_node;
			struct sched_autonuma *sa = rq->curr->sched_autonuma;
			_autonuma_node = ACCESS_ONCE(sa->autonuma_node);
			if (_autonuma_node != autonuma_node)
				return false;
			if (idle_cpu(cpu) && rq->avg_idle >=
			    sysctl_sched_migration_cost)
				return false;
		}
	}
	return true;
}

void sched_autonuma_dump_mm(void)
{
	int nid, cpu;
	struct cpumask x;
	cpumask_setall(&x);
	for_each_online_node(nid) {
		for_each_cpu(cpu, cpumask_of_node(nid)) {
			struct rq *rq = cpu_rq(cpu);
			struct mm_struct *mm = rq->curr->mm;
			int nr = 0, cpux;
			if (!cpumask_test_cpu(cpu, &x))
				continue;
			for_each_cpu(cpux, cpumask_of_node(nid)) {
				struct rq *rqx = cpu_rq(cpux);
				if (rqx->curr->mm == mm) {
					nr++;
					cpumask_clear_cpu(cpux, &x);
				}
			}
			printk("nid %d mm %p nr %d\n", nid, mm, nr);
		}
	}
}
