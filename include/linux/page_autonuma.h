#ifndef _LINUX_PAGE_AUTONUMA_H
#define _LINUX_PAGE_AUTONUMA_H

#include <linux/autonuma_flags.h>

#if defined(CONFIG_AUTONUMA) && !defined(CONFIG_SPARSEMEM)
extern void __init page_autonuma_init_flatmem(void);
#else
static inline void __init page_autonuma_init_flatmem(void) {}
#endif

#ifdef CONFIG_AUTONUMA

extern void __meminit page_autonuma_map_init(struct page *page,
					     struct page_autonuma *page_autonuma,
					     int nr_pages);

#ifdef CONFIG_SPARSEMEM
#define PAGE_AUTONUMA_SIZE (sizeof(struct page_autonuma))
#define SECTION_PAGE_AUTONUMA_SIZE (PAGE_AUTONUMA_SIZE *	\
				    PAGES_PER_SECTION)
#endif

extern void __meminit pgdat_autonuma_init(struct pglist_data *);

#else /* CONFIG_AUTONUMA */

#ifdef CONFIG_SPARSEMEM
struct page_autonuma;
#define PAGE_AUTONUMA_SIZE 0
#define SECTION_PAGE_AUTONUMA_SIZE 0
#endif /* CONFIG_SPARSEMEM */

static inline void pgdat_autonuma_init(struct pglist_data *pgdat) {}

#endif /* CONFIG_AUTONUMA */

#ifdef CONFIG_SPARSEMEM
extern struct page_autonuma * __meminit __kmalloc_section_page_autonuma(int nid,
									unsigned long nr_pages);
extern void __kfree_section_page_autonuma(struct page_autonuma *page_autonuma,
					  unsigned long nr_pages);
extern void __init sparse_early_page_autonuma_alloc_node(struct page_autonuma **page_autonuma_map,
							 unsigned long pnum_begin,
							 unsigned long pnum_end,
							 unsigned long map_count,
							 int nodeid);
#endif

#endif /* _LINUX_PAGE_AUTONUMA_H */
