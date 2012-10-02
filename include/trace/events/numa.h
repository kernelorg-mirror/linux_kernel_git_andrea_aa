#undef TRACE_SYSTEM
#define TRACE_SYSTEM numa

#if !defined(_TRACE_NUMA_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_NUMA_H

#include <linux/types.h>
#include <linux/nodemask.h>
#include <linux/tracepoint.h>

/*
 * The tracepoint output can be more complete if the kernel is compiled
 * with CONFIG_MM_OWNER and the mm for which pages were migrated is passed
 * to the tracepoint.
 */
#ifdef CONFIG_MM_OWNER
#include <linux/sched.h>
#define __assign_task(entry, mm)					\
	({								\
		if (mm) {						\
			struct task_struct *t = mm->owner;		\
			memcpy(entry->comm, t->comm, TASK_COMM_LEN);	\
			entry->pid = t->pid;				\
			entry->prio = t->prio;				\
		} else {						\
			strcpy(entry->comm, "no-mm-info");		\
			entry->pid = 0;					\
			entry->prio = 0;				\
		}							\
	})
#else
#define __assign_task(entry, mm)					\
	({								\
		strcpy(entry->comm, "no-mm-owner");			\
		entry->pid = 0;						\
		entry->prio = 0;					\
	})
#endif

#define __nodemask_copy(dst, __src)					\
	({								\
		nodemask_t *_src = (nodemask_t *)__src;			\
		unsigned char *src = (unsigned char *)nodes_addr(*_src);\
		int i = sizeof(nodemask_t);				\
		memset(dst, 0, i);					\
		if (src) {						\
			while (--i >= 0 && !src[i])			\
				;					\
			if (++i)					\
				memcpy(dst, src, i);			\
		} else							\
			i = 0;						\
		i;							\
	})

/*
 * trace_numa_migratepages_begin - begin tracepoint for node to node page migration
 * @mm:        pointer to the mm for which pages will be migrated,
 *             or NULL if it's unknown
 * @pagelist:  pointer to the list of pages to be migrated
 * @src:       from node, or -1 if it's unknown
 * @dest:      to node, or -1 if it's unknown
 *
 * This is called before migrate_pages() to state what we're trying to
 * migrate, and to act as the start marker.
 */
TRACE_EVENT(numa_migratepages_begin,

	TP_PROTO(struct mm_struct *mm,
		 struct list_head *pagelist,
		 int src, int dest),

	TP_ARGS(mm, pagelist, src, dest),

	TP_STRUCT__entry(
		__array(char, comm, TASK_COMM_LEN)
		__field(pid_t, pid)
		__field(int, prio)
		__field(unsigned int, to_migrate)
		__field(int, src)
		__field(int, dest)
	),

	TP_fast_assign(
		__assign_task(__entry, mm);
		__entry->to_migrate = __list_len(pagelist);
		__entry->src = src;
		__entry->dest = dest;
	),

	TP_printk("comm=%s pid=%d prio=%d to_migrate=%u src=%d dest=%d",
		__entry->comm, __entry->pid, __entry->prio,
		__entry->to_migrate, __entry->src, __entry->dest)
);

/*
 * trace_numa_migratepages_nodemask_begin - begin tracepoint for node to node page migration
 * @mm:        pointer to the mm for which pages will be migrated,
 *             or NULL if it's unknown
 * @pagelist:  pointer to the list of pages to be migrated
 * @src:       from nodemask, or NULL if it's unknown
 * @dest:      to nodemask, or NULL if it's unknown
 *
 * This is a variant of trace_numa_migratepages_begin where src and dest are
 * nodemasks rather than single nodes.
 */

TRACE_EVENT(numa_migratepages_nodemask_begin,

	TP_PROTO(struct mm_struct *mm,
		 struct list_head *pagelist,
		 nodemask_t *src, nodemask_t *dest),

	TP_ARGS(mm, pagelist, src, dest),

	TP_STRUCT__entry(
		__array(char, comm, TASK_COMM_LEN)
		__field(pid_t, pid)
		__field(int, prio)
		__field(unsigned int, to_migrate)
		__field(int, masklen)
		__array(unsigned char, src, (int)sizeof(nodemask_t))
		__array(unsigned char, dest, (int)sizeof(nodemask_t))
	),

	TP_fast_assign(
		int srclen, destlen;

		__assign_task(__entry, mm);
		__entry->to_migrate = __list_len(pagelist);

		srclen = __nodemask_copy(__entry->src, src);
		destlen = __nodemask_copy(__entry->dest, dest);
		__entry->masklen = max(srclen, destlen);
	),

	TP_printk("comm=%s pid=%d prio=%d to_migrate=%u src=%s dest=%s",
		__entry->comm, __entry->pid, __entry->prio,
		__entry->to_migrate,
		__print_hex(__entry->src, __entry->masklen),
		__print_hex(__entry->dest, __entry->masklen))
);

/*
 * trace_numa_migratepages_end - end tracepoint for node to node page migration
 * @nr_failed: return value of migrate_pages()
 *
 * This is called after migrate_pages() to see the number of failed pages,
 * and to act as the end marker.
 */
DECLARE_EVENT_CLASS(numa_migratepages_end_template,

	TP_PROTO(unsigned int nr_failed),

	TP_ARGS(nr_failed),

	TP_STRUCT__entry(
		__field(unsigned int, nr_failed)
	),

	TP_fast_assign(
		__entry->nr_failed = nr_failed;
	),

	TP_printk("nr_failed=%u", __entry->nr_failed)
);

DEFINE_EVENT(numa_migratepages_end_template, numa_migratepages_end,

	TP_PROTO(unsigned int nr_failed),

	TP_ARGS(nr_failed)
);

DEFINE_EVENT(numa_migratepages_end_template, numa_migratepages_nodemask_end,

	TP_PROTO(unsigned int nr_failed),

	TP_ARGS(nr_failed)
);

#undef __assign_task
#undef __nodemask_copy
#endif /* _TRACE_NUMA_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
