#ifndef _LINUX_AUTONUMA_FLAGS_H
#define _LINUX_AUTONUMA_FLAGS_H

enum autonuma_flag {
	AUTONUMA_FLAG,
	AUTONUMA_IMPOSSIBLE,
	AUTONUMA_DEBUG_FLAG,
	AUTONUMA_SCHED_LOAD_BALANCE_STRICT_FLAG,
	AUTONUMA_SCHED_CLONE_RESET_FLAG,
	AUTONUMA_SCHED_FORK_RESET_FLAG,
	AUTONUMA_SCAN_PMD_FLAG,
	AUTONUMA_SCAN_USE_WORKING_SET_FLAG,
	AUTONUMA_MIGRATE_DEFER_FLAG,
};

extern unsigned long autonuma_flags;

static bool inline autonuma_enabled(void)
{
	return !!test_bit(AUTONUMA_FLAG, &autonuma_flags);
}

static bool inline autonuma_debug(void)
{
	return !!test_bit(AUTONUMA_DEBUG_FLAG, &autonuma_flags);
}

static bool inline autonuma_load_balance_strict(void)
{
	return !!test_bit(AUTONUMA_SCHED_LOAD_BALANCE_STRICT_FLAG,
			  &autonuma_flags);
}

static bool inline autonuma_sched_clone_reset(void)
{
	return !!test_bit(AUTONUMA_SCHED_CLONE_RESET_FLAG,
			  &autonuma_flags);
}

static bool inline autonuma_sched_fork_reset(void)
{
	return !!test_bit(AUTONUMA_SCHED_FORK_RESET_FLAG,
			  &autonuma_flags);
}

static bool inline autonuma_scan_pmd(void)
{
	return !!test_bit(AUTONUMA_SCAN_PMD_FLAG, &autonuma_flags);
}

static bool inline autonuma_scan_use_working_set(void)
{
	return !!test_bit(AUTONUMA_SCAN_USE_WORKING_SET_FLAG,
			  &autonuma_flags);
}

static bool inline autonuma_migrate_defer(void)
{
	return !!test_bit(AUTONUMA_MIGRATE_DEFER_FLAG, &autonuma_flags);
}

#endif /* _LINUX_AUTONUMA_FLAGS_H */
