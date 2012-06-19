#include <linux/mm.h>
#include <linux/memory.h>
#include <linux/vmalloc.h>
#include <linux/autonuma.h>
#include <linux/page_autonuma.h>
#include <linux/bootmem.h>

void __meminit page_autonuma_map_init(struct page *page,
				      struct page_autonuma *page_autonuma,
				      int nr_pages)
{
	struct page *end;
	for (end = page + nr_pages; page < end; page++, page_autonuma++) {
		page_autonuma->autonuma_last_nid = -1;
		page_autonuma->autonuma_migrate_nid = -1;
	}
}

static void __meminit __pgdat_autonuma_init(struct pglist_data *pgdat)
{
	int node_iter;

	/* verify the per-page page_autonuma 12 byte fixed cost */
	BUILD_BUG_ON((unsigned long) &((struct page_autonuma *)0)[1] != 12);

	spin_lock_init(&pgdat->autonuma_lock);
	init_waitqueue_head(&pgdat->autonuma_knuma_migrated_wait);
	pgdat->autonuma_nr_migrate_pages = 0;

	/* initialize autonuma_possible() */
	if (num_possible_nodes() <= 1)
		clear_bit(AUTONUMA_POSSIBLE_FLAG, &autonuma_flags);

	/* noautonuma early param may also clear AUTONUMA_POSSIBLE_FLAG */
	if (autonuma_possible())
		for_each_node(node_iter) {
			struct autonuma_list_head *head;
			head = &pgdat->autonuma_migrate_head[node_iter];
			AUTONUMA_INIT_LIST_HEAD(head);
		}
}

#if !defined(CONFIG_SPARSEMEM)

static unsigned long total_usage;

void __meminit pgdat_autonuma_init(struct pglist_data *pgdat)
{
	__pgdat_autonuma_init(pgdat);
	pgdat->node_page_autonuma = NULL;
}

struct page_autonuma *lookup_page_autonuma(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long offset;
	struct page_autonuma *base;

	base = NODE_DATA(page_to_nid(page))->node_page_autonuma;
#ifdef CONFIG_DEBUG_VM
	/*
	 * The sanity checks the page allocator does upon freeing a
	 * page can reach here before the page_autonuma arrays are
	 * allocated when feeding a range of pages to the allocator
	 * for the first time during bootup or memory hotplug.
	 */
	if (unlikely(!base))
		return NULL;
#endif
	offset = pfn - NODE_DATA(page_to_nid(page))->node_start_pfn;
	return base + offset;
}

static int __init alloc_node_page_autonuma(int nid)
{
	struct page_autonuma *base;
	unsigned long table_size;
	unsigned long nr_pages;

	nr_pages = NODE_DATA(nid)->node_spanned_pages;
	if (!nr_pages)
		return 0;

	table_size = sizeof(struct page_autonuma) * nr_pages;

	base = __alloc_bootmem_node_nopanic(NODE_DATA(nid),
			table_size, PAGE_SIZE, __pa(MAX_DMA_ADDRESS));
	if (!base)
		return -ENOMEM;
	NODE_DATA(nid)->node_page_autonuma = base;
	total_usage += table_size;
	page_autonuma_map_init(NODE_DATA(nid)->node_mem_map, base, nr_pages);
	return 0;
}

void __init page_autonuma_init_flatmem(void)
{

	int nid, fail;

	/* __pgdat_autonuma_init initialized autonuma_possible() */
	if (!autonuma_possible())
		return;

	for_each_online_node(nid)  {
		fail = alloc_node_page_autonuma(nid);
		if (fail)
			goto fail;
	}
	printk(KERN_INFO "allocated %lu KBytes of page_autonuma\n",
	       total_usage >> 10);
	printk(KERN_INFO "please try the 'noautonuma' option if you"
	" don't want to allocate page_autonuma memory\n");
	return;
fail:
	printk(KERN_CRIT "allocation of page_autonuma failed.\n");
	printk(KERN_CRIT "please try the 'noautonuma' boot option\n");
	panic("Out of memory");
}

#else /* CONFIG_SPARSEMEM */

struct page_autonuma *lookup_page_autonuma(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct mem_section *section = __pfn_to_section(pfn);

#ifdef CONFIG_DEBUG_VM
	/*
	 * The sanity checks the page allocator does upon freeing a
	 * page can reach here before the page_autonuma arrays are
	 * allocated when feeding a range of pages to the allocator
	 * for the first time during bootup or memory hotplug.
	 */
	if (!section->section_page_autonuma)
		return NULL;
#endif
	return section->section_page_autonuma + pfn;
}

void __meminit pgdat_autonuma_init(struct pglist_data *pgdat)
{
	/* memsection must be a power of two */
	BUILD_BUG_ON(sizeof(struct mem_section) &
		     (sizeof(struct mem_section)-1));

	__pgdat_autonuma_init(pgdat);
}

struct page_autonuma * __meminit __kmalloc_section_page_autonuma(int nid,
								 unsigned long nr_pages)
{
	struct page_autonuma *ret;
	struct page *page;
	unsigned long memmap_size = PAGE_AUTONUMA_SIZE * nr_pages;

	page = alloc_pages_node(nid, GFP_KERNEL|__GFP_NOWARN,
				get_order(memmap_size));
	if (page)
		goto got_map_page_autonuma;

	ret = vmalloc(memmap_size);
	if (ret)
		goto out;

	return NULL;
got_map_page_autonuma:
	ret = (struct page_autonuma *)pfn_to_kaddr(page_to_pfn(page));
out:
	return ret;
}

void __kfree_section_page_autonuma(struct page_autonuma *page_autonuma,
				   unsigned long nr_pages)
{
	if (is_vmalloc_addr(page_autonuma))
		vfree(page_autonuma);
	else
		free_pages((unsigned long)page_autonuma,
			   get_order(PAGE_AUTONUMA_SIZE * nr_pages));
}

static struct page_autonuma __init *sparse_page_autonuma_map_populate(unsigned long pnum,
								      int nid)
{
	struct page_autonuma *map;
	unsigned long size;

	map = alloc_remap(nid, SECTION_PAGE_AUTONUMA_SIZE);
	if (map)
		return map;

	size = PAGE_ALIGN(SECTION_PAGE_AUTONUMA_SIZE);
	map = __alloc_bootmem_node_high(NODE_DATA(nid), size,
					PAGE_SIZE, __pa(MAX_DMA_ADDRESS));
	return map;
}

void __init sparse_early_page_autonuma_alloc_node(struct page_autonuma **page_autonuma_map,
						  unsigned long pnum_begin,
						  unsigned long pnum_end,
						  unsigned long map_count,
						  int nodeid)
{
	void *map;
	unsigned long pnum;
	unsigned long size = SECTION_PAGE_AUTONUMA_SIZE;

	map = alloc_remap(nodeid, size * map_count);
	if (map) {
		for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
			if (!present_section_nr(pnum))
				continue;
			page_autonuma_map[pnum] = map;
			map += size;
		}
		return;
	}

	size = PAGE_ALIGN(size);
	map = __alloc_bootmem_node_high(NODE_DATA(nodeid), size * map_count,
					PAGE_SIZE, __pa(MAX_DMA_ADDRESS));
	if (map) {
		for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
			if (!present_section_nr(pnum))
				continue;
			page_autonuma_map[pnum] = map;
			map += size;
		}
		return;
	}

	/* fallback */
	for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
		struct mem_section *ms;

		if (!present_section_nr(pnum))
			continue;
		page_autonuma_map[pnum] = sparse_page_autonuma_map_populate(pnum, nodeid);
		if (page_autonuma_map[pnum])
			continue;
		ms = __nr_to_section(pnum);
		printk(KERN_ERR "%s: sparsemem page_autonuma map backing failed "
		       "some memory will not be available.\n", __func__);
	}
}

#endif /* CONFIG_SPARSEMEM */
