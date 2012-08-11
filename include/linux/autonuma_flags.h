#ifndef _LINUX_AUTONUMA_FLAGS_H
#define _LINUX_AUTONUMA_FLAGS_H

/*
 * If CONFIG_AUTONUMA=n this file isn't included and only
 * autonuma_possible() is defined (as false) in page_autonuma.h to
 * allow optimizing away at compile time blocks of common code without
 * using #ifdefs.
 */
#ifndef CONFIG_AUTONUMA
#error "autonuma flags included by mistake"
#endif

enum autonuma_flag {
	/*
	 * Set if the kernel wasn't passed the "noautonuma" boot
	 * parameter and the hardware is NUMA. If AutoNUMA is not
	 * possible the value of all other flags becomes irrelevant
	 * (they will never be checked) and AutoNUMA can't be enabled.
	 *
	 * No defaults: depends on hardware discovery and "noautonuma"
	 * early param.
	 */
	AUTONUMA_POSSIBLE_FLAG,
	/*
	 * If AutoNUMA is possible, this defines if AutoNUMA is
	 * currently enabled or disabled. It can be toggled at runtime
	 * through sysfs.
	 *
	 * The default depends on CONFIG_AUTONUMA_DEFAULT_ENABLED.
	 */
	AUTONUMA_ENABLED_FLAG,
	/*
	 * If set through sysfs this will print lots of debug info
	 * about the AutoNUMA activities in the kernel logs.
	 *
	 * Default not set.
	 */
	AUTONUMA_DEBUG_FLAG,
	/*
	 * This defines if CFS should prioritize between load
	 * balancing fairness or NUMA affinity, if there are no idle
	 * CPUs available. If this flag is set AutoNUMA will
	 * prioritize on NUMA affinity and it will disregard
	 * inter-node fairness.
	 *
	 * Default not set.
	 */
	AUTONUMA_SCHED_LOAD_BALANCE_STRICT_FLAG,
	/*
	 * This flag defines if the task/mm_autonuma statistics should
	 * be inherithed from the parent task/process or instead if
	 * they should be cleared at every fork/clone. The
	 * task/mm_autonuma statistics are always cleared across
	 * execve and there's no way to disable that.
	 *
	 * Default set.
	 */
	AUTONUMA_SCHED_RESET_FLAG,
	/*
	 * If set, this tells knuma_scand to trigger NUMA hinting page
	 * faults at the pmd level instead of the pte level. This
	 * reduces the number of NUMA hinting faults potentially
	 * saving CPU time. It reduces the accuracy of the
	 * task_autonuma statistics (it doesn't change the accuracy of
	 * the mm_autonuma statistics if the mm_working_set mode is
	 * not set). This flag can be toggled through sysfs as
	 * runtime.
	 *
	 * This flag does not affect AutoNUMA with transparent
	 * hugepages (THP). With THP the NUMA hinting page faults
	 * always happen at the pmd level, regardless of the setting
	 * of this flag. Note: there is no reduction in accuracy of
	 * task_autonuma statistics with THP.
	 *
	 * Default set.
	 */
	AUTONUMA_SCAN_PMD_FLAG,
	/*
	 * If set, knuma_migrated will wake up in the middle of each
	 * knuma_scand pass, regardless of how many pages have been
	 * already queued. If not set knuma_migrated will wake up as
	 * soon as the number of pages in the migration LRU reached a
	 * certain threshold.
	 *
	 * Default not set.
	 */
	AUTONUMA_MIGRATE_DEFER_FLAG,
	/*
	 * If set, a page must successfully pass a last_nid check
	 * before it can be migrated even if it's the very first NUMA
	 * hinting page fault occurring on the page. If not set, the
	 * first NUMA hinting page fault of a newly allocated page
	 * will always pass the last_nid check.
	 *
	 * If not set a newly started workload can converge quicker,
	 * but it may incur in more false positive migrations before
	 * reaching convergence.
	 *
	 * Default not set.
	 */
	AUTONUMA_MIGRATE_ALLOW_FIRST_FAULT_FLAG,
	/*
	 * If set, mm_autonuma will represent a working set estimation
	 * of the memory used by the process over the last knuma_scand
	 * pass.
	 *
	 * If not set, mm_autonuma will represent all (not shared)
	 * memory eligible for automatic migration mapped by the
	 * process.
	 *
	 * Default set.
	 */
	AUTONUMA_MM_WORKING_SET_FLAG,
};

extern unsigned long autonuma_flags;

static inline bool autonuma_possible(void)
{
	return test_bit(AUTONUMA_POSSIBLE_FLAG, &autonuma_flags);
}

static inline bool autonuma_enabled(void)
{
	return test_bit(AUTONUMA_ENABLED_FLAG, &autonuma_flags);
}

static inline bool autonuma_debug(void)
{
	return test_bit(AUTONUMA_DEBUG_FLAG, &autonuma_flags);
}

static inline bool autonuma_sched_load_balance_strict(void)
{
	return test_bit(AUTONUMA_SCHED_LOAD_BALANCE_STRICT_FLAG,
			&autonuma_flags);
}

static inline bool autonuma_sched_reset(void)
{
	return test_bit(AUTONUMA_SCHED_RESET_FLAG,
			&autonuma_flags);
}

static inline bool autonuma_scan_pmd(void)
{
	return test_bit(AUTONUMA_SCAN_PMD_FLAG, &autonuma_flags);
}

static inline bool autonuma_migrate_defer(void)
{
	return test_bit(AUTONUMA_MIGRATE_DEFER_FLAG, &autonuma_flags);
}

static inline bool autonuma_migrate_allow_first_fault(void)
{
	return test_bit(AUTONUMA_MIGRATE_ALLOW_FIRST_FAULT_FLAG,
			&autonuma_flags);
}

static inline bool autonuma_mm_working_set(void)
{
	return test_bit(AUTONUMA_MM_WORKING_SET_FLAG,
			&autonuma_flags);
}

#endif /* _LINUX_AUTONUMA_FLAGS_H */
