/* Copyright (c) 2016 Google Inc
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * #mem, memory diagnostics (arenas and slabs)
 */

#include <ns.h>
#include <kmalloc.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <syscall.h>
#include <sys/queue.h>

struct dev mem_devtab;

static char *devname(void)
{
	return mem_devtab.name;
}

enum {
	Qdir,
	Qarena_stats,
	Qslab_stats,
	Qfree,
	Qkmemstat,
};

static struct dirtab mem_dir[] = {
	{".", {Qdir, 0, QTDIR}, 0, DMDIR | 0555},
	{"arena_stats", {Qarena_stats, 0, QTFILE}, 0, 0444},
	{"slab_stats", {Qslab_stats, 0, QTFILE}, 0, 0444},
	{"free", {Qfree, 0, QTFILE}, 0, 0444},
	{"kmemstat", {Qkmemstat, 0, QTFILE}, 0, 0444},
};

static struct chan *mem_attach(char *spec)
{
	return devattach(devname(), spec);
}

static struct walkqid *mem_walk(struct chan *c, struct chan *nc, char **name,
								unsigned int nname)
{
	return devwalk(c, nc, name, nname, mem_dir, ARRAY_SIZE(mem_dir), devgen);
}

static size_t mem_stat(struct chan *c, uint8_t *db, size_t n)
{
	return devstat(c, db, n, mem_dir, ARRAY_SIZE(mem_dir), devgen);
}

/* Prints arena's stats to the sza, starting at sofar.  Returns the new sofar.*/
static size_t fetch_arena_stats(struct arena *arena, struct sized_alloc *sza,
                                size_t sofar)
{
	struct btag *bt_i;
	struct rb_node *rb_i;
	struct arena *a_i;
	struct kmem_cache *kc_i;

	size_t nr_allocs = 0;
	size_t nr_imports = 0;
	size_t amt_alloc = 0;
	size_t amt_free = 0;
	size_t amt_imported = 0;
	size_t empty_hash_chain = 0;
	size_t longest_hash_chain = 0;

	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Arena: %s (%p)\n--------------\n", arena->name, arena);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\tquantum: %d, qcache_max: %d\n", arena->quantum,
	                  arena->qcache_max);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\tsource: %s\n",
	                  arena->source ? arena->source->name : "none");
	spin_lock_irqsave(&arena->lock);
	for (int i = 0; i < ARENA_NR_FREE_LISTS; i++) {
		int j = 0;

		if (!BSD_LIST_EMPTY(&arena->free_segs[i])) {
			sofar += snprintf(sza->buf + sofar, sza->size - sofar,
			                  "\tList of [2^%d - 2^%d):\n", i, i + 1);
			BSD_LIST_FOREACH(bt_i, &arena->free_segs[i], misc_link)
				j++;
			sofar += snprintf(sza->buf + sofar, sza->size - sofar,
			                  "\t\tNr free segs: %d\n", j);
		}
	}
	for (int i = 0; i < arena->hh.nr_hash_lists; i++) {
		int j = 0;

		if (BSD_LIST_EMPTY(&arena->alloc_hash[i]))
			empty_hash_chain++;
		BSD_LIST_FOREACH(bt_i, &arena->alloc_hash[i], misc_link)
			j++;
		longest_hash_chain = MAX(longest_hash_chain, j);
	}
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\tSegments:\n\t--------------\n");
	for (rb_i = rb_first(&arena->all_segs); rb_i; rb_i = rb_next(rb_i)) {
		bt_i = container_of(rb_i, struct btag, all_link);
		if (bt_i->status == BTAG_SPAN) {
			nr_imports++;
			amt_imported += bt_i->size;
		}
		if (bt_i->status == BTAG_FREE)
			amt_free += bt_i->size;
		if (bt_i->status == BTAG_ALLOC) {
			nr_allocs++;
			amt_alloc += bt_i->size;
		}
	}
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\tStats:\n\t-----------------\n");
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\t\tAmt free: %llu (%p)\n", amt_free, amt_free);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\t\tAmt alloc: %llu (%p), nr allocs %d\n",
	                  amt_alloc, amt_alloc, nr_allocs);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\t\tAmt total segs: %llu, amt alloc segs %llu\n",
	                  arena->amt_total_segs, arena->amt_alloc_segs);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\t\tAmt imported: %llu (%p), nr imports %d\n",
					  amt_imported, amt_imported, nr_imports);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\t\tNr hash %d, empty hash: %d, longest hash %d\n",
	                  arena->hh.nr_hash_lists, empty_hash_chain,
					  longest_hash_chain);
	spin_unlock_irqsave(&arena->lock);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\tImporting Arenas:\n\t-----------------\n");
	TAILQ_FOREACH(a_i, &arena->__importing_arenas, import_link)
		sofar += snprintf(sza->buf + sofar, sza->size - sofar,
		                  "\t\t%s\n", a_i->name);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\tImporting Slabs:\n\t-----------------\n");
	TAILQ_FOREACH(kc_i, &arena->__importing_slabs, import_link)
		sofar += snprintf(sza->buf + sofar, sza->size - sofar,
		                  "\t\t%s\n", kc_i->name);
	return sofar;
}

static struct sized_alloc *build_arena_stats(void)
{
	struct sized_alloc *sza;
	size_t sofar = 0;
	size_t alloc_amt = 0;
	struct arena *a_i;

	qlock(&arenas_and_slabs_lock);
	/* Rough guess about how many chars per arena we'll need. */
	TAILQ_FOREACH(a_i, &all_arenas, next)
		alloc_amt += 1000;
	sza = sized_kzmalloc(alloc_amt, MEM_WAIT);
	TAILQ_FOREACH(a_i, &all_arenas, next)
		sofar = fetch_arena_stats(a_i, sza, sofar);
	qunlock(&arenas_and_slabs_lock);
	return sza;
}

/* Prints arena's stats to the sza, starting at sofar.  Returns the new sofar.*/
static size_t fetch_slab_stats(struct kmem_cache *kc, struct sized_alloc *sza,
                               size_t sofar)
{
	struct kmem_slab *s_i;
	struct kmem_bufctl *bc_i;

	size_t nr_unalloc_objs = 0;
	size_t empty_hash_chain = 0;
	size_t longest_hash_chain = 0;

	spin_lock_irqsave(&kc->cache_lock);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "\nKmem_cache: %s\n---------------------\n", kc->name);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Source: %s\n", kc->source->name);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Objsize (incl align): %d\n", kc->obj_size);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Align: %d\n", kc->align);
	TAILQ_FOREACH(s_i, &kc->empty_slab_list, link) {
		assert(!s_i->num_busy_obj);
		nr_unalloc_objs += s_i->num_total_obj;
	}
	TAILQ_FOREACH(s_i, &kc->partial_slab_list, link)
		nr_unalloc_objs += s_i->num_total_obj - s_i->num_busy_obj;
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Nr unallocated in slab layer: %lu\n", nr_unalloc_objs);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Nr allocated from slab layer: %d\n", kc->nr_cur_alloc);
	for (int i = 0; i < kc->hh.nr_hash_lists; i++) {
		int j = 0;

		if (BSD_LIST_EMPTY(&kc->alloc_hash[i]))
			empty_hash_chain++;
		BSD_LIST_FOREACH(bc_i, &kc->alloc_hash[i], link)
			j++;
		longest_hash_chain = MAX(longest_hash_chain, j);
	}
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Nr hash %d, empty hash: %d, longest hash %d, loadlim %d\n",
	                  kc->hh.nr_hash_lists, empty_hash_chain,
					  longest_hash_chain, kc->hh.load_limit);
	spin_unlock_irqsave(&kc->cache_lock);
	spin_lock_irqsave(&kc->depot.lock);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Depot magsize: %d\n", kc->depot.magsize);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Nr empty mags: %d\n", kc->depot.nr_empty);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Nr non-empty mags: %d\n", kc->depot.nr_not_empty);
	spin_unlock_irqsave(&kc->depot.lock);
	return sofar;
}

static struct sized_alloc *build_slab_stats(void)
{
	struct sized_alloc *sza;
	size_t sofar = 0;
	size_t alloc_amt = 0;
	struct kmem_cache *kc_i;

	qlock(&arenas_and_slabs_lock);
	TAILQ_FOREACH(kc_i, &all_kmem_caches, all_kmc_link)
		alloc_amt += 500;
	sza = sized_kzmalloc(alloc_amt, MEM_WAIT);
	TAILQ_FOREACH(kc_i, &all_kmem_caches, all_kmc_link)
		sofar = fetch_slab_stats(kc_i, sza, sofar);
	qunlock(&arenas_and_slabs_lock);
	return sza;
}

static struct sized_alloc *build_free(void)
{
	struct arena *a_i;
	struct sized_alloc *sza;
	size_t sofar = 0;
	size_t amt_total = 0;
	size_t amt_alloc = 0;

	sza = sized_kzmalloc(500, MEM_WAIT);
	qlock(&arenas_and_slabs_lock);
	TAILQ_FOREACH(a_i, &all_arenas, next) {
		if (!a_i->is_base)
			continue;
		amt_total += a_i->amt_total_segs;
		amt_alloc += a_i->amt_alloc_segs;
	}
	qunlock(&arenas_and_slabs_lock);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Total Memory : %15llu\n", amt_total);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Used Memory  : %15llu\n", amt_alloc);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  "Free Memory  : %15llu\n", amt_total - amt_alloc);
	return sza;
}

#define KMEMSTAT_NAME			30
#define KMEMSTAT_OBJSIZE		8
#define KMEMSTAT_TOTAL			15
#define KMEMSTAT_ALLOCED		15
#define KMEMSTAT_NR_ALLOCS		12
#define KMEMSTAT_LINE_LN (8 + KMEMSTAT_NAME + KMEMSTAT_OBJSIZE + KMEMSTAT_TOTAL\
                          + KMEMSTAT_ALLOCED + KMEMSTAT_NR_ALLOCS)

const char kmemstat_fmt[]     = "%-*s: %c :%*llu:%*llu:%*llu:%*llu\n";
const char kmemstat_hdr_fmt[] = "%-*s:Typ:%*s:%*s:%*s:%*s\n";

static size_t fetch_arena_line(struct arena *arena, struct sized_alloc *sza,
                               size_t sofar, int indent)
{
	for (int i = 0; i < indent; i++)
		sofar += snprintf(sza->buf + sofar, sza->size - sofar, "    ");
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  kmemstat_fmt,
					  KMEMSTAT_NAME - indent * 4, arena->name,
					  'A',
					  KMEMSTAT_OBJSIZE, arena->quantum,
					  KMEMSTAT_TOTAL, arena->amt_total_segs,
					  KMEMSTAT_ALLOCED, arena->amt_alloc_segs,
					  KMEMSTAT_NR_ALLOCS, arena->nr_allocs_ever);
	return sofar;
}

static size_t fetch_slab_line(struct kmem_cache *kc, struct sized_alloc *sza,
                              size_t sofar, int indent)
{
	struct kmem_pcpu_cache *pcc;
	struct kmem_slab *s_i;
	size_t nr_unalloc_objs = 0;
	size_t nr_allocs_ever = 0;

	spin_lock_irqsave(&kc->cache_lock);
	TAILQ_FOREACH(s_i, &kc->empty_slab_list, link)
		nr_unalloc_objs += s_i->num_total_obj;
	TAILQ_FOREACH(s_i, &kc->partial_slab_list, link)
		nr_unalloc_objs += s_i->num_total_obj - s_i->num_busy_obj;
	nr_allocs_ever = kc->nr_direct_allocs_ever;
	spin_unlock_irqsave(&kc->cache_lock);
	/* Lockless peak at the pcpu state */
	for (int i = 0; i < kmc_nr_pcpu_caches(); i++) {
		pcc = &kc->pcpu_caches[i];
		nr_allocs_ever += pcc->nr_allocs_ever;
	}

	for (int i = 0; i < indent; i++)
		sofar += snprintf(sza->buf + sofar, sza->size - sofar, "    ");
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  kmemstat_fmt,
					  KMEMSTAT_NAME - indent * 4, kc->name,
					  'S',
					  KMEMSTAT_OBJSIZE, kc->obj_size,
					  KMEMSTAT_TOTAL, kc->obj_size * (nr_unalloc_objs +
					                                  kc->nr_cur_alloc),
					  KMEMSTAT_ALLOCED, kc->obj_size * kc->nr_cur_alloc,
					  KMEMSTAT_NR_ALLOCS, nr_allocs_ever);
	return sofar;
}

static size_t fetch_arena_and_kids(struct arena *arena, struct sized_alloc *sza,
                                   size_t sofar, int indent)
{
	struct arena *a_i;
	struct kmem_cache *kc_i;

	sofar = fetch_arena_line(arena, sza, sofar, indent);
	TAILQ_FOREACH(a_i, &arena->__importing_arenas, import_link)
		sofar = fetch_arena_and_kids(a_i, sza, sofar, indent + 1);
	TAILQ_FOREACH(kc_i, &arena->__importing_slabs, import_link)
		sofar = fetch_slab_line(kc_i, sza, sofar, indent + 1);
	return sofar;
}

static struct sized_alloc *build_kmemstat(void)
{
	struct arena *a_i;
	struct kmem_cache *kc_i;
	struct sized_alloc *sza;
	size_t sofar = 0;
	size_t alloc_amt = 100;

	qlock(&arenas_and_slabs_lock);
	TAILQ_FOREACH(a_i, &all_arenas, next)
		alloc_amt += 100;
	TAILQ_FOREACH(kc_i, &all_kmem_caches, all_kmc_link)
		alloc_amt += 100;
	sza = sized_kzmalloc(alloc_amt, MEM_WAIT);
	sofar += snprintf(sza->buf + sofar, sza->size - sofar,
	                  kmemstat_hdr_fmt,
					  KMEMSTAT_NAME, "Arena/Slab Name",
					  KMEMSTAT_OBJSIZE, "Objsize",
					  KMEMSTAT_TOTAL, "Total Amt",
					  KMEMSTAT_ALLOCED, "Alloc Amt",
					  KMEMSTAT_NR_ALLOCS, "Allocs Ever");
	for (int i = 0; i < KMEMSTAT_LINE_LN; i++)
		sofar += snprintf(sza->buf + sofar, sza->size - sofar, "-");
	sofar += snprintf(sza->buf + sofar, sza->size - sofar, "\n");
	TAILQ_FOREACH(a_i, &all_arenas, next) {
		if (a_i->source)
			continue;
		sofar = fetch_arena_and_kids(a_i, sza, sofar, 0);
	}
	qunlock(&arenas_and_slabs_lock);
	return sza;
}

static struct chan *mem_open(struct chan *c, int omode)
{
	if (c->qid.type & QTDIR) {
		if (openmode(omode) != O_READ)
			error(EPERM, "Tried opening directory not read-only");
	}
	switch (c->qid.path) {
	case Qarena_stats:
		c->synth_buf = build_arena_stats();
		break;
	case Qslab_stats:
		c->synth_buf = build_slab_stats();
		break;
	case Qfree:
		c->synth_buf = build_free();
		break;
	case Qkmemstat:
		c->synth_buf = build_kmemstat();
		break;
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void mem_close(struct chan *c)
{
	if (!(c->flag & COPEN))
		return;
	switch (c->qid.path) {
	case Qarena_stats:
	case Qslab_stats:
	case Qfree:
	case Qkmemstat:
		kfree(c->synth_buf);
		break;
	}
}

static size_t mem_read(struct chan *c, void *ubuf, size_t n, off64_t offset)
{
	struct sized_alloc *sza;

	switch (c->qid.path) {
	case Qdir:
		return devdirread(c, ubuf, n, mem_dir, ARRAY_SIZE(mem_dir),
						  devgen);
	case Qarena_stats:
	case Qslab_stats:
	case Qfree:
	case Qkmemstat:
		sza = c->synth_buf;
		return readmem(offset, ubuf, n, sza->buf, sza->size);
	default:
		panic("Bad Qid %p!", c->qid.path);
	}
	return -1;
}

static size_t mem_write(struct chan *c, void *ubuf, size_t n, off64_t offset)
{
	switch (c->qid.path) {
	default:
		error(EFAIL, "Unable to write to %s", devname());
	}
	return n;
}

struct dev mem_devtab __devtab = {
	.name = "mem",
	.reset = devreset,
	.init = devinit,
	.shutdown = devshutdown,
	.attach = mem_attach,
	.walk = mem_walk,
	.stat = mem_stat,
	.open = mem_open,
	.create = devcreate,
	.close = mem_close,
	.read = mem_read,
	.bread = devbread,
	.write = mem_write,
	.bwrite = devbwrite,
	.remove = devremove,
	.wstat = devwstat,
	.power = devpower,
	.chaninfo = devchaninfo,
};
