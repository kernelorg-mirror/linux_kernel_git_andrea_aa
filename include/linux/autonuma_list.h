#ifndef __AUTONUMA_LIST_H
#define __AUTONUMA_LIST_H

#include <linux/types.h>
#include <linux/kernel.h>

typedef uint32_t autonuma_list_entry;
#define AUTONUMA_LIST_MAX_PFN_OFFSET	(AUTONUMA_LIST_HEAD-3)
#define AUTONUMA_LIST_POISON1		(AUTONUMA_LIST_HEAD-2)
#define AUTONUMA_LIST_POISON2		(AUTONUMA_LIST_HEAD-1)
#define AUTONUMA_LIST_HEAD		((uint32_t)UINT_MAX)

struct autonuma_list_head {
	autonuma_list_entry anl_next_pfn;
	autonuma_list_entry anl_prev_pfn;
};

static inline void AUTONUMA_INIT_LIST_HEAD(struct autonuma_list_head *anl)
{
	anl->anl_next_pfn = AUTONUMA_LIST_HEAD;
	anl->anl_prev_pfn = AUTONUMA_LIST_HEAD;
}

/* abstraction conversion methods */
extern struct page *autonuma_list_entry_to_page(int nid,
					autonuma_list_entry pfn_offset);
extern autonuma_list_entry autonuma_page_to_list_entry(int page_nid,
						       struct page *page);
extern struct autonuma_list_head *__autonuma_list_head(int page_nid,
					struct autonuma_list_head *head,
					autonuma_list_entry pfn_offset);

extern bool __autonuma_list_add(int page_nid,
				struct page *page,
				struct autonuma_list_head *head,
				autonuma_list_entry prev,
				autonuma_list_entry next);

/*
 * autonuma_list_add - add a new entry
 *
 * Insert a new entry after the specified head.
 */
static inline bool autonuma_list_add(int page_nid,
				     struct page *page,
				     autonuma_list_entry entry,
				     struct autonuma_list_head *head)
{
	struct autonuma_list_head *entry_head;
	entry_head = __autonuma_list_head(page_nid, head, entry);
	return __autonuma_list_add(page_nid, page, head,
				   entry, entry_head->anl_next_pfn);
}

/*
 * autonuma_list_add_tail - add a new entry
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
static inline bool autonuma_list_add_tail(int page_nid,
					  struct page *page,
					  autonuma_list_entry entry,
					  struct autonuma_list_head *head)
{
	struct autonuma_list_head *entry_head;
	entry_head = __autonuma_list_head(page_nid, head, entry);
	return __autonuma_list_add(page_nid, page, head,
				   entry_head->anl_prev_pfn, entry);
}

/*
 * autonuma_list_del - deletes entry from list.
 * @entry: the element to delete from the list.
 */
extern void autonuma_list_del(int page_nid,
			      struct autonuma_list_head *entry,
			      struct autonuma_list_head *head);

static inline bool autonuma_list_empty(const struct autonuma_list_head *head)
{
	return ACCESS_ONCE(head->anl_next_pfn) == AUTONUMA_LIST_HEAD;
}

/* safe to call only when the list cannot change under us */
extern bool autonuma_list_empty_debug(const struct autonuma_list_head *head);

#if 0 /* not needed so far */
/*
 * autonuma_list_is_singular - tests whether a list has just one entry.
 * @head: the list to test.
 */
static inline int autonuma_list_is_singular(const struct autonuma_list_head *head)
{
	return !autonuma_list_empty_debug(head) &&
		(head->anl_next_pfn == head->anl_prev_pfn);
}
#endif

#endif /* __AUTONUMA_LIST_H */
