/*
 * Copyright 2006, Red Hat, Inc., Dave Jones
 * Copyright 2012, Red Hat, Inc.
 * Released under the General Public License (GPL).
 *
 * This file contains the linked list implementations for
 * autonuma migration lists.
 */

#include <linux/mm.h>
#include <linux/autonuma.h>

/*
 * Insert a new entry between two known consecutive entries.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 *
 * return true if succeeded, or false if the (page_nid, pfn_offset)
 * pair couldn't represent the pfn and the list_add didn't succeed.
 */
bool __autonuma_list_add(int page_nid,
			 struct page *page,
			 struct autonuma_list_head *head,
			 autonuma_list_entry prev,
			 autonuma_list_entry next)
{
	autonuma_list_entry new;

	VM_BUG_ON(page_nid != page_to_nid(page));
	new = autonuma_page_to_list_entry(page_nid, page);
	if (new > AUTONUMA_LIST_MAX_PFN_OFFSET)
		return false;

	WARN(new == prev || new == next,
	     "autonuma_list_add double add: new=%u, prev=%u, next=%u.\n",
	     new, prev, next);

	__autonuma_list_head(page_nid, head, next)->anl_prev_pfn = new;
	__autonuma_list_head(page_nid, head, new)->anl_next_pfn = next;
	__autonuma_list_head(page_nid, head, new)->anl_prev_pfn = prev;
	__autonuma_list_head(page_nid, head, prev)->anl_next_pfn = new;
	return true;
}

static inline void __autonuma_list_del_entry(int page_nid,
					     struct autonuma_list_head *entry,
					     struct autonuma_list_head *head)
{
	autonuma_list_entry prev, next;

	next = entry->anl_next_pfn;
	prev = entry->anl_prev_pfn;

	if (WARN(next == AUTONUMA_LIST_POISON1,
		 "autonuma_list_del corruption, "
		 "%p->anl_next_pfn is AUTONUMA_LIST_POISON1 (%u)\n",
		entry, AUTONUMA_LIST_POISON1) ||
	    WARN(prev == AUTONUMA_LIST_POISON2,
		"autonuma_list_del corruption, "
		 "%p->anl_prev_pfn is AUTONUMA_LIST_POISON2 (%u)\n",
		entry, AUTONUMA_LIST_POISON2))
		return;

	__autonuma_list_head(page_nid, head, next)->anl_prev_pfn = prev;
	__autonuma_list_head(page_nid, head, prev)->anl_next_pfn = next;
}

/*
 * autonuma_list_del - deletes entry from list.
 *
 * Note: autonuma_list_empty on entry does not return true after this,
 * the entry is in an undefined state.
 */
void autonuma_list_del(int page_nid, struct autonuma_list_head *entry,
		       struct autonuma_list_head *head)
{
	__autonuma_list_del_entry(page_nid, entry, head);
	entry->anl_next_pfn = AUTONUMA_LIST_POISON1;
	entry->anl_prev_pfn = AUTONUMA_LIST_POISON2;
}

/*
 * autonuma_list_empty - tests whether a list is empty
 * @head: the list to test.
 */
bool autonuma_list_empty_debug(const struct autonuma_list_head *head)
{
	bool ret = false;
	if (head->anl_next_pfn == AUTONUMA_LIST_HEAD) {
		ret = true;
		BUG_ON(head->anl_prev_pfn != AUTONUMA_LIST_HEAD);
	}
	return ret;
}

/* abstraction conversion methods */

static inline struct page *__autonuma_list_entry_to_page(int page_nid,
							 autonuma_list_entry pfn_offset)
{
	struct pglist_data *pgdat = NODE_DATA(page_nid);
	unsigned long pfn = pgdat->node_start_pfn + pfn_offset;
	BUG_ON(pfn_offset >= pgdat->node_spanned_pages);
	return pfn_to_page(pfn);
}

struct page *autonuma_list_entry_to_page(int page_nid,
					 autonuma_list_entry pfn_offset)
{
	VM_BUG_ON(page_nid < 0);
	BUG_ON(pfn_offset == AUTONUMA_LIST_POISON1);
	BUG_ON(pfn_offset == AUTONUMA_LIST_POISON2);
	BUG_ON(pfn_offset == AUTONUMA_LIST_HEAD);
	return __autonuma_list_entry_to_page(page_nid, pfn_offset);
}

/*
 * returns a value above AUTONUMA_LIST_MAX_PFN_OFFSET if the pfn is
 * located a too big offset from the start of the node and cannot be
 * represented by the (page_nid, pfn_offset) pair.
 */
autonuma_list_entry autonuma_page_to_list_entry(int page_nid,
						struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct pglist_data *pgdat = NODE_DATA(page_nid);
	VM_BUG_ON(page_nid != page_to_nid(page));
	BUG_ON(pfn < pgdat->node_start_pfn);
	BUG_ON(pfn >= pgdat->node_start_pfn + pgdat->node_spanned_pages);
	pfn -= pgdat->node_start_pfn;
	if (pfn > AUTONUMA_LIST_MAX_PFN_OFFSET) {
		WARN_ONCE(1, "autonuma_page_to_list_entry: "
			  "pfn_offset  %lu, pgdat %p, "
			  "pgdat->node_start_pfn %lu\n",
			  pfn, pgdat, pgdat->node_start_pfn);
		/*
		 * Any value bigger than AUTONUMA_LIST_MAX_PFN_OFFSET
		 * will work as an error retval, but better pick one
		 * that will cause noise if computed wrong by the
		 * caller.
		 */
		return AUTONUMA_LIST_POISON1;
	}
	return pfn; /* convert to uint16_t without losing information */
}

static inline struct autonuma_list_head *____autonuma_list_head(int page_nid,
					autonuma_list_entry pfn_offset)
{
	struct pglist_data *pgdat = NODE_DATA(page_nid);
	unsigned long pfn = pgdat->node_start_pfn + pfn_offset;
	struct page *page = pfn_to_page(pfn);
	struct page_autonuma *page_autonuma = lookup_page_autonuma(page);
	return &page_autonuma->autonuma_migrate_node;
}

struct autonuma_list_head *__autonuma_list_head(int page_nid,
					struct autonuma_list_head *head,
					autonuma_list_entry pfn_offset)
{
	VM_BUG_ON(page_nid < 0);
	BUG_ON(pfn_offset == AUTONUMA_LIST_POISON1);
	BUG_ON(pfn_offset == AUTONUMA_LIST_POISON2);
	if (pfn_offset != AUTONUMA_LIST_HEAD)
		return ____autonuma_list_head(page_nid, pfn_offset);
	else
		return head;
}
