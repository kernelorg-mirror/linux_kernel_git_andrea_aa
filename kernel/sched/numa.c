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

/*
 * autonuma_balance_cpu_stop() is a callback to be invoked by
 * stop_one_cpu_nowait(). It is used by __sched_autonuma_balance() to
 * migrate the tasks to the selected_cpu, from softirq context.
 */
static int autonuma_balance_cpu_stop(void *data)
{
	struct rq *src_rq = data;
	int src_cpu = cpu_of(src_rq);
	int dst_cpu = src_rq->autonuma_balance_dst_cpu;
	struct task_struct *p = src_rq->autonuma_balance_task;
	struct rq *dst_rq = cpu_rq(dst_cpu);

	raw_spin_lock_irq(&p->pi_lock);
	raw_spin_lock(&src_rq->lock);

	/* Make sure the selected cpu hasn't gone down in the meanwhile */
	if (unlikely(src_cpu != smp_processor_id() ||
		     !src_rq->autonuma_balance))
		goto out_unlock;

	/* Check if the affinity changed in the meanwhile */
	if (!cpumask_test_cpu(dst_cpu, tsk_cpus_allowed(p)))
		goto out_unlock;

	/* Is the task to migrate still there? */
	if (task_cpu(p) != src_cpu)
		goto out_unlock;

	BUG_ON(src_rq == dst_rq);

	/* Prepare to move the task from src_rq to dst_rq */
	double_lock_balance(src_rq, dst_rq);

	/*
	 * Supposedly pi_lock should have been enough but some code
	 * seems to call __set_task_cpu without pi_lock.
	 */
	if (task_cpu(p) != src_cpu)
		goto out_double_unlock;

	/*
	 * If the task is not on a rq, the autonuma_nid will take
	 * care of the NUMA affinity at the next wake-up.
	 */
	if (p->on_rq) {
		deactivate_task(src_rq, p, 0);
		set_task_cpu(p, dst_cpu);
		activate_task(dst_rq, p, 0);
		check_preempt_curr(dst_rq, p, 0);
	}

out_double_unlock:
	double_unlock_balance(src_rq, dst_rq);
out_unlock:
	src_rq->autonuma_balance = false;
	raw_spin_unlock(&src_rq->lock);
	/* spinlocks acts as barrier() so p is stored local on the stack */
	raw_spin_unlock_irq(&p->pi_lock);
	put_task_struct(p);
	return 0;
}

#define AUTONUMA_BALANCE_SCALE 1000

enum {
	W_TYPE_THREAD,
	W_TYPE_PROCESS,
};

/*
 * This function __sched_autonuma_balance() is responsible for
 * deciding which is the best CPU each process should be running on
 * according to the NUMA statistics collected in mm->mm_autonuma and
 * tsk->task_autonuma.
 *
 * The core math that evaluates the current CPU against the CPUs of
 * all _other_ nodes is this:
 *
 *	if (w_nid > w_other && w_nid > w_this_nid)
 *		weight = w_nid - w_other + w_nid - w_this_nid;
 *
 * w_nid: NUMA affinity of the current thread/process if run on the
 * other CPU.
 *
 * w_other: NUMA affinity of the other thread/process if run on the
 * other CPU.
 *
 * w_this_nid: NUMA affinity of the current thread/process if run on
 * the current CPU.
 *
 * weight: combined NUMA affinity benefit in moving the current
 * thread/process to the other CPU taking into account both the higher
 * NUMA affinity of the current process if run on the other CPU, and
 * the increase in NUMA affinity in the other CPU by replacing the
 * other process.
 *
 * We run the above math on every CPU not part of the current NUMA
 * node, and we compare the current process against the other
 * processes running in the other CPUs in the remote NUMA nodes. The
 * objective is to select the cpu (in selected_cpu) with a bigger
 * "weight". The bigger the "weight" the biggest gain we'll get by
 * moving the current process to the selected_cpu (not only the
 * biggest immediate CPU gain but also the fewer async memory
 * migrations that will be required to reach full convergence
 * later). If we select a cpu we migrate the current process to it.
 *
 * Checking that the current process has higher NUMA affinity than the
 * other process on the other CPU (w_nid > w_other) and not only that
 * the current process has higher NUMA affinity on the other CPU than
 * on the current CPU (w_nid > w_this_nid) completely avoids ping
 * pongs and ensures (temporary) convergence of the algorithm (at
 * least from a CPU standpoint).
 *
 * It's then up to the idle balancing code that will run as soon as
 * the current CPU goes idle to pick the other process and move it
 * here (or in some other idle CPU if any).
 *
 * By only evaluating running processes against running processes we
 * avoid interfering with the CFS stock active idle balancing, which
 * is critical to optimal performance with HT enabled. (getting HT
 * wrong is worse than running on remote memory so the active idle
 * balancing has priority)
 *
 * Idle balancing and all other CFS load balancing become NUMA
 * affinity aware through the introduction of
 * sched_autonuma_can_migrate_task(). CFS searches CPUs in the task's
 * autonuma_nid first when it needs to find idle CPUs during idle
 * balancing or tasks to pick during load balancing.
 *
 * The task's autonuma_nid is the node selected by
 * __sched_autonuma_balance() when it migrates a task to the
 * selected_cpu in the selected_nid.
 *
 * Once a process/thread has been moved to another node, closer to the
 * much of memory it has recently accessed, any memory for that task
 * not in the new node moves slowly (asynchronously in the background)
 * to the new node. This is done by the knuma_migratedN (where the
 * suffix N is the node id) daemon described in mm/autonuma.c.
 *
 * One important thing of this logic is how the three crucial
 * variables of the core math (w_nid/w_other/w_this_nid as defined at
 * the top of this comment) are going to change depending on whether
 * the other CPU is running a thread of the current process, or a
 * thread of a different process.
 *
 * A simple example is required. Given the following:
 * - 2 processes
 * - 4 threads per process
 * - 2 NUMA nodes
 * - 4 CPUS per NUMA node
 *
 * Because the 8 threads belong to 2 different processes, by using the
 * process statistics when comparing threads of different processes,
 * we will converge reliably and quickly to a configuration where the
 * 1st process is entirely contained in one node and the 2nd process
 * in the other node.
 *
 * If all threads only use thread local memory (no sharing of memory
 * between the threads), it wouldn't matter if we use per-thread or
 * per-mm statistics for w_nid/w_other/w_this_nid. We could then use
 * per-thread statistics all the time.
 *
 * But clearly with threads it's expected to get some sharing of
 * memory. To avoid false sharing it's better to keep all threads of
 * the same process in the same node (or if they don't fit in a single
 * node, in as fewer nodes as possible). This is why we have to use
 * processes statistics in w_nid/w_other/w_this_nid when comparing
 * threads of different processes. Why instead do we have to use
 * thread statistics when comparing threads of the same process? This
 * should be obvious if you're still reading (hint: the mm statistics
 * are identical for threads of the same process). If some process
 * doesn't fit in one node, the thread statistics will then distribute
 * the threads to the best nodes within the group of nodes where the
 * process is contained.
 *
 * False sharing in the above sentence (and generally in AutoNUMA
 * context) is intended as virtual memory accessed simultaneously (or
 * frequently) by threads running in CPUs of different nodes. This
 * doesn't refer to shared memory as in tmpfs, but it refers to
 * CLONE_VM instead. If the threads access the same memory from CPUs
 * of different nodes it means the memory accesses will be NUMA local
 * for some thread and NUMA remote for some other thread. The only way
 * to avoid NUMA false sharing is to schedule all threads accessing
 * the same memory in the same node (which may or may not be possible,
 * if it's not possible because there aren't enough CPUs in the node,
 * the threads should be scheduled in as few nodes as possible and the
 * nodes distance should be the lowest possible).
 *
 * This is an example of the CPU layout after the startup of 2
 * processes with 12 threads each. This is some of the logs you will
 * find in `dmesg` after running:
 *
 *	echo 1 >/sys/kernel/mm/autonuma/debug
 *
 * nid is the node id
 * mm is the pointer to the mm structure (kind of the "ID" of the process)
 * nr is the number of threads of that belongs to that process in that node id.
 *
 * This dumps the raw content of the CPUs' runqueues, it doesn't show
 * kernel threads (the kernel thread dumping the below stats is
 * clearly using one CPU, hence only 23 CPUs are dumped, clearly the
 * debug mode can be improved but it's good enough to see what's going
 * on).
 *
 * nid 0 mm ffff880433367b80 nr 6
 * nid 0 mm ffff880433367480 nr 5
 * nid 1 mm ffff880433367b80 nr 6
 * nid 1 mm ffff880433367480 nr 6
 *
 * Now, the process with mm == ffff880433367b80 has 6 threads in node0
 * and 6 threads in node1, while the process with mm ==
 * ffff880433367480 has 5 threads in node0 and 6 threads running in
 * node1.
 *
 * And after a few seconds it becomes:
 *
 * nid 0 mm ffff880433367b80 nr 12
 * nid 1 mm ffff880433367480 nr 11
 *
 * Now, 12 threads of one process are running on node 0 and 11 threads
 * of the other process are running on node 1.
 *
 * Before scanning all other CPUs' runqueues to compute the above
 * math, we also verify that the current CPU is not already in the
 * preferred NUMA node from the point of view of both the process
 * statistics and the thread statistics. In such case we can return to
 * the caller without having to check any other CPUs' runqueues
 * because full convergence has been already reached.
 *
 * This algorithm might be expanded to take all runnable processes
 * into account but examining just the currently running processes is
 * a good enough approximation because some runnable processes may run
 * only for a short time so statistically there will always be a bias
 * on the processes that uses most the of the CPU. This is ideal
 * because it doesn't matter if NUMA balancing isn't optimal for
 * processes that run only for a short time.
 *
 * This function is invoked at the same frequency and in the same
 * location of the CFS load balancer and only if the CPU is not
 * idle. The rest of the time we depend on CFS to keep sticking to the
 * current CPU or to prioritize on the CPUs in the selected_nid
 * (recorded in the task's autonuma_nid field).
 */
void __sched_autonuma_balance(void)
{
	int cpu, nid, selected_cpu, selected_nid, selected_nid_mm;
	int this_nid = numa_node_id();
	int this_cpu = smp_processor_id();
	/*
	 * w_t: node thread weight
	 * w_t_t: total sum of all node thread weights
	 * w_m: node mm/process weight
	 * w_m_t: total sum of all node mm/process weights
	 */
	unsigned long w_t, w_t_t, w_m, w_m_t;
	unsigned long w_t_max, w_m_max;
	unsigned long weight_max, weight;
	long s_w_nid = -1, s_w_this_nid = -1, s_w_other = -1;
	int s_w_type = -1;
	struct cpumask *allowed;
	struct task_struct *p = current;
	struct task_autonuma *task_autonuma = p->task_autonuma;
	struct rq *rq;

	/* per-cpu statically allocated in runqueues */
	long *task_numa_weight;
	long *mm_numa_weight;

	if (!task_autonuma || !p->mm)
		return;

	if (!autonuma_enabled()) {
		if (task_autonuma->autonuma_nid != -1)
			task_autonuma->autonuma_nid = -1;
		return;
	}

	allowed = tsk_cpus_allowed(p);

	/*
	 * Do nothing if the task had no numa hinting page faults yet
	 * or if the mm hasn't been fully scanned by knuma_scand yet.
	 */
	w_t_t = task_autonuma->task_numa_fault_tot;
	if (!w_t_t)
		return;
	w_m_t = ACCESS_ONCE(p->mm->mm_autonuma->mm_numa_fault_tot);
	if (!w_m_t)
		return;

	/*
	 * The following two arrays will hold the NUMA affinity weight
	 * information for the current process if scheduled on the
	 * given NUMA node.
	 *
	 * mm_numa_weight[nid] - mm NUMA affinity weight for the NUMA node
	 * task_numa_weight[nid] - task NUMA affinity weight for the NUMA node
	 */
	rq = cpu_rq(this_cpu);
	task_numa_weight = rq->task_numa_weight;
	mm_numa_weight = rq->mm_numa_weight;

	w_t_max = w_m_max = 0;
	selected_nid = selected_nid_mm = -1;
	for_each_online_node(nid) {
		w_m = ACCESS_ONCE(p->mm->mm_autonuma->mm_numa_fault[nid]);
		w_t = task_autonuma->task_numa_fault[nid];
		if (w_m > w_m_t)
			w_m_t = w_m;
		mm_numa_weight[nid] = w_m*AUTONUMA_BALANCE_SCALE/w_m_t;
		if (w_t > w_t_t)
			w_t_t = w_t;
		task_numa_weight[nid] = w_t*AUTONUMA_BALANCE_SCALE/w_t_t;
		if (mm_numa_weight[nid] > w_m_max) {
			w_m_max = mm_numa_weight[nid];
			selected_nid_mm = nid;
		}
		if (task_numa_weight[nid] > w_t_max) {
			w_t_max = task_numa_weight[nid];
			selected_nid = nid;
		}
	}
	/*
	 * Skip the more expensive loop below if we selected the
	 * current NUMA node based on both mm and task NUMA affinity
	 * weights.
	 */
	if (selected_nid == this_nid && selected_nid_mm == selected_nid) {
		if (task_autonuma->autonuma_nid != selected_nid)
			task_autonuma->autonuma_nid = selected_nid;
		return;
	}

	selected_cpu = this_cpu;
	selected_nid = this_nid;

	weight = weight_max = 0;

	/* check that the following raw_spin_lock_irq is safe */
	BUG_ON(irqs_disabled());

	for_each_online_node(nid) {
		/*
		 * Calculate the NUMA affinity weight for all CPUs
		 * that the current process could be migrated to.
		 * Note: From a NUMA affinity standpoint, it doesn't
		 * make sense to migrate the process to another CPU of
		 * the current NUMA node.
		 */
		if (nid == this_nid)
			continue;
		for_each_cpu_and(cpu, cpumask_of_node(nid), allowed) {
			long w_nid, w_this_nid, w_other;
			int w_type;
			struct mm_struct *mm;
			rq = cpu_rq(cpu);
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
			/*
			 * Grab the w_m/w_t/w_m_t/w_t_t of the
			 * processes running in the other CPUs to
			 * compute w_other.
			 */
			raw_spin_lock_irq(&rq->lock);
			/* recheck after implicit barrier() */
			mm = rq->curr->mm;
			if (!mm) {
				raw_spin_unlock_irq(&rq->lock);
				continue;
			}
			w_m_t = ACCESS_ONCE(mm->mm_autonuma->mm_numa_fault_tot);
			w_t_t = rq->curr->task_autonuma->task_numa_fault_tot;
			if (!w_m_t || !w_t_t) {
				raw_spin_unlock_irq(&rq->lock);
				continue;
			}
			w_m = ACCESS_ONCE(mm->mm_autonuma->mm_numa_fault[nid]);
			w_t = rq->curr->task_autonuma->task_numa_fault[nid];
			raw_spin_unlock_irq(&rq->lock);
			/*
			 * Generate the w_nid/w_this_nid from the
			 * pre-computed mm/task_numa_weight[] and
			 * compute w_other using the w_m/w_t info
			 * collected from the other process.
			 */
			if (mm == p->mm) {
				if (w_t > w_t_t)
					w_t_t = w_t;
				w_other = w_t*AUTONUMA_BALANCE_SCALE/w_t_t;
				w_nid = task_numa_weight[nid];
				w_this_nid = task_numa_weight[this_nid];
				w_type = W_TYPE_THREAD;
			} else {
				if (w_m > w_m_t)
					w_m_t = w_m;
				w_other = w_m*AUTONUMA_BALANCE_SCALE/w_m_t;
				w_nid = mm_numa_weight[nid];
				w_this_nid = mm_numa_weight[this_nid];
				w_type = W_TYPE_PROCESS;
			}

			/*
			 * Finally check if there's a combined gain in
			 * NUMA affinity. If there is and it's the
			 * biggest weight seen so far, record its
			 * weight and select this NUMA remote "cpu" as
			 * candidate migration destination.
			 */
			if (w_nid > w_other && w_nid > w_this_nid) {
				weight = w_nid - w_other + w_nid - w_this_nid;

				if (weight > weight_max) {
					weight_max = weight;
					selected_cpu = cpu;
					selected_nid = nid;

					s_w_other = w_other;
					s_w_nid = w_nid;
					s_w_this_nid = w_this_nid;
					s_w_type = w_type;
				}
			}
		}
	}

	if (task_autonuma->autonuma_nid != selected_nid)
		task_autonuma->autonuma_nid = selected_nid;
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
			       p->mm, p->pid, this_nid, selected_nid,
			       this_cpu, selected_cpu,
			       s_w_other, s_w_nid, s_w_this_nid,
			       w_type_str);
		}
		BUG_ON(this_nid == selected_nid);
		goto found;
	}

	return;

found:
	rq = cpu_rq(this_cpu);

	/*
	 * autonuma_balance synchronizes accesses to
	 * autonuma_balance_work. Once set, it's cleared by the
	 * callback once the migration work is finished.
	 */
	raw_spin_lock_irq(&rq->lock);
	if (rq->autonuma_balance) {
		raw_spin_unlock_irq(&rq->lock);
		return;
	}
	rq->autonuma_balance = true;
	raw_spin_unlock_irq(&rq->lock);

	rq->autonuma_balance_dst_cpu = selected_cpu;
	rq->autonuma_balance_task = p;
	get_task_struct(p);

	stop_one_cpu_nowait(this_cpu,
			    autonuma_balance_cpu_stop, rq,
			    &rq->autonuma_balance_work);
#ifdef __ia64__
#error "NOTE: tlb_migrate_finish won't run here, review before deleting"
#endif
}

/*
 * The function sched_autonuma_can_migrate_task is called by CFS
 * can_migrate_task() to prioritize on the task's autonuma_nid. It is
 * called during load_balancing, idle balancing and in general
 * before any task CPU migration event happens.
 *
 * The caller first scans the CFS migration candidate tasks passing a
 * not zero numa parameter, to skip tasks without AutoNUMA affinity
 * (according to the tasks's autonuma_nid). If no task can be
 * migrated in the first scan, a second scan is run with a zero numa
 * parameter.
 *
 * If load_balance_strict is enabled, AutoNUMA will only allow
 * migration of tasks for idle balancing purposes (the idle balancing
 * of CFS is never altered by AutoNUMA). In the not strict mode the
 * load balancing is not altered and the AutoNUMA affinity is
 * disregarded in favor of higher fairness. The load_balance_strict
 * knob is runtime tunable in sysfs.
 *
 * If load_balance_strict is enabled, it tends to partition the
 * system. In turn it may reduce the scheduler fairness across NUMA
 * nodes, but it should deliver higher global performance.
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

/*
 * sched_autonuma_dump_mm is a purely debugging function called at
 * regular intervals when /sys/kernel/mm/autonuma/debug is
 * enabled. This prints in the kernel logs how the threads and
 * processes are distributed in all NUMA nodes to easily check if the
 * threads of the same processes are converging in the same
 * nodes. This won't take into account kernel threads and because it
 * runs itself from a kernel thread it won't show what was running in
 * the current CPU, but it's simple and good enough to get what we
 * need in the debug logs. This function can be disabled or deleted
 * later.
 */
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
