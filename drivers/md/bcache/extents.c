/*
 * Copyright (C) 2010 Kent Overstreet <kent.overstreet@gmail.com>
 *
 * Uses a block device as cache for other block devices; optimized for SSDs.
 * All allocation is done in buckets, which should match the erase block size
 * of the device.
 *
 * Buckets containing cached data are kept on a heap sorted by priority;
 * bucket priority is increased on cache hit, and periodically all the buckets
 * on the heap have their priority scaled down. This currently is just used as
 * an LRU but in the future should allow for more intelligent heuristics.
 *
 * Buckets have an 8 bit counter; freeing is accomplished by incrementing the
 * counter. Garbage collection is used to remove stale pointers.
 *
 * Indexing is done via a btree; nodes are not necessarily fully sorted, rather
 * as keys are inserted we only sort the pages that have not yet been written.
 * When garbage collection is run, we resort the entire node.
 *
 * All configuration is done via sysfs; see Documentation/bcache.txt.
 */

#include "bcache.h"
#include "btree.h"
#include "debug.h"
#include "extents.h"
#include "writeback.h"

static void sort_key_next(struct btree_iter *iter,
			  struct btree_iter_set *i)
{
	i->k = bkey_next(i->k);

	if (i->k == i->end)
		*i = iter->data[--iter->used];
}

bool bch_key_sort_cmp(struct btree_iter_set l,
		      struct btree_iter_set r)
{
	int64_t c = bkey_cmp(l.k, r.k);

	return c ? c > 0 : l.k < r.k;
}

void bch_key_sort_fixup(struct btree_iter *iter)
{
	while (iter->used > 1) {
		struct btree_iter_set *top = iter->data, *i = top + 1;

		if (iter->used > 2 &&
		    bch_key_sort_cmp(i[0], i[1]))
			i++;

		/*
		 * If this key and the next key don't compare equal, we're done.
		 */

		if (bkey_cmp(top->k, i->k))
			break;

		/*
		 * If they do compare equal, the newer key overwrote the older
		 * key and we need to drop the older key.
		 *
		 * bch_key_sort_cmp() ensures that when keys compare equal the
		 * newer key comes first; so i->k is older than top->k and we
		 * drop i->k.
		 */

		i->k = bkey_next(i->k);

		if (i->k == i->end)
			*i = iter->data[--iter->used];

		heap_sift(iter, i - top, bch_key_sort_cmp);
	}
}

/* Btree ptrs */

static bool __ptr_invalid(struct cache_set *c, const struct bkey *k)
{
	unsigned i;

	for (i = 0; i < KEY_PTRS(k); i++)
		if (ptr_available(c, k, i)) {
			struct cache *ca = PTR_CACHE(c, k, i);
			size_t bucket = PTR_BUCKET_NR(c, k, i);
			size_t r = bucket_remainder(c, PTR_OFFSET(k, i));

			if (KEY_SIZE(k) + r > c->sb.bucket_size ||
			    bucket <  ca->sb.first_bucket ||
			    bucket >= ca->sb.nbuckets)
				return true;
		}

	return false;
}

void bch_btree_ptr_sort_fixup(struct btree_iter *iter)
{
	while (iter->used > 1) {
		struct btree_iter_set *top = iter->data, *i = top + 1;

		if (iter->used > 2 &&
		    bch_key_sort_cmp(i[0], i[1]))
			i++;

		/* Old style freeing keys - don't check for duplicates */
		if (!bkey_cmp(top->k, &ZERO_KEY))
			break;

		/*
		 * If this key and the next key don't compare equal, we're done.
		 */

		if (bkey_cmp(top->k, i->k))
			break;

		/*
		 * If they do compare equal, the newer key overwrote the older
		 * key and we need to drop the older key.
		 *
		 * bch_key_sort_cmp() ensures that when keys compare equal the
		 * newer key comes first; so i->k is older than top->k and we
		 * drop i->k.
		 */

		i->k = bkey_next(i->k);

		if (i->k == i->end)
			*i = iter->data[--iter->used];

		heap_sift(iter, i - top, bch_key_sort_cmp);
	}
}

bool __bch_btree_ptr_invalid(struct cache_set *c, const struct bkey *k)
{
	char buf[80];

	if (KEY_CACHED(k))
		goto bad;

	if (!KEY_DELETED(k) && !KEY_PTRS(k))
		goto bad;

	if (bkey_cmp(k, &ZERO_KEY) && !KEY_SIZE(k)) /* old style freeing keys */
		goto bad;

	if (__ptr_invalid(c, k))
		goto bad;

	return false;
bad:
	bch_bkey_to_text(buf, sizeof(buf), k);
	cache_bug(c, "spotted btree ptr %s: %s", buf, bch_ptr_status(c, k));
	return true;
}

bool bch_btree_ptr_invalid(struct btree_keys *bk, const struct bkey *k)
{
	struct btree *b = container_of(bk, struct btree, keys);
	return __bch_btree_ptr_invalid(b->c, k);
}

static bool btree_ptr_bad_expensive(struct btree *b, const struct bkey *k)
{
	unsigned i;
	char buf[80];
	struct bucket *g;

	if (mutex_trylock(&b->c->bucket_lock)) {
		for (i = 0; i < KEY_PTRS(k); i++)
			if (ptr_available(b->c, k, i)) {
				g = PTR_BUCKET(b->c, k, i);

				if (KEY_CACHED(k) ||
				    (b->c->gc_mark_valid &&
				     GC_MARK(g) != GC_MARK_METADATA))
					goto err;
			}

		mutex_unlock(&b->c->bucket_lock);
	}

	return false;
err:
	mutex_unlock(&b->c->bucket_lock);
	bch_bkey_to_text(buf, sizeof(buf), k);
	btree_bug(b,
"inconsistent btree pointer %s: bucket %li pin %i prio %i gen %i last_gc %i mark %llu gc_gen %i",
		  buf, PTR_BUCKET_NR(b->c, k, i), atomic_read(&g->pin),
		  g->prio, g->gen, g->last_gc, GC_MARK(g), g->gc_gen);
	return true;
}

bool bch_btree_ptr_bad(struct btree_keys *bk, const struct bkey *k)
{
	struct btree *b = container_of(bk, struct btree, keys);
	unsigned i;

	if (KEY_DELETED(k) ||
	    !bkey_cmp(k, &ZERO_KEY) || /* old style freeing keys */
	    __bch_btree_ptr_invalid(b->c, k))
		return true;

	for (i = 0; i < KEY_PTRS(k); i++)
		if (!ptr_available(b->c, k, i))
			return true;

	if (expensive_debug_checks(b->c) &&
	    btree_ptr_bad_expensive(b, k))
		return true;

	return false;
}

/* Extents */

/*
 * Returns true if l > r - unless l == r, in which case returns true if l is
 * older than r.
 *
 * Necessary for btree_sort_fixup() - if there are multiple keys that compare
 * equal in different sets, we have to process them newest to oldest.
 */
bool bch_extent_sort_cmp(struct btree_iter_set l,
			 struct btree_iter_set r)
{
	int64_t c = bkey_cmp(&START_KEY(l.k), &START_KEY(r.k));

	return c ? c > 0 : l.k < r.k;
}

void bch_extent_sort_fixup(struct btree_iter *iter)
{
	while (iter->used > 1) {
		struct btree_iter_set *top = iter->data, *i = top + 1;

		if (iter->used > 2 &&
		    bch_extent_sort_cmp(i[0], i[1]))
			i++;

		if (bkey_cmp(top->k, &START_KEY(i->k)) <= 0)
			break;

		if (!KEY_SIZE(i->k)) {
			sort_key_next(iter, i);
			heap_sift(iter, i - top, bch_extent_sort_cmp);
			continue;
		}

		if (top->k > i->k) {
			if (bkey_cmp(top->k, i->k) >= 0)
				sort_key_next(iter, i);
			else
				bch_cut_front(top->k, i->k);

			heap_sift(iter, i - top, bch_extent_sort_cmp);
		} else {
			/* can't happen because of comparison func */
			BUG_ON(!bkey_cmp(&START_KEY(top->k), &START_KEY(i->k)));
			bch_cut_back(&START_KEY(i->k), top->k);
		}
	}
}

bool bch_extent_invalid(struct btree_keys *bk, const struct bkey *k)
{
	struct btree *b = container_of(bk, struct btree, keys);
	char buf[80];

	if (!KEY_SIZE(k))
		return true;

	if (KEY_SIZE(k) > KEY_OFFSET(k))
		goto bad;

	if (__ptr_invalid(b->c, k))
		goto bad;

	return false;
bad:
	bch_bkey_to_text(buf, sizeof(buf), k);
	cache_bug(b->c, "spotted extent %s: %s", buf, bch_ptr_status(b->c, k));
	return true;
}

static bool bch_extent_bad_expensive(struct btree *b, const struct bkey *k,
				     unsigned ptr)
{
	struct bucket *g = PTR_BUCKET(b->c, k, ptr);
	char buf[80];

	if (mutex_trylock(&b->c->bucket_lock)) {
		if (b->c->gc_mark_valid &&
		    ((GC_MARK(g) != GC_MARK_DIRTY &&
		      !KEY_CACHED(k)) ||
		     GC_MARK(g) == GC_MARK_METADATA))
			goto err;

		mutex_unlock(&b->c->bucket_lock);
	}

	return false;
err:
	mutex_unlock(&b->c->bucket_lock);
	bch_bkey_to_text(buf, sizeof(buf), k);
	btree_bug(b,
"inconsistent extent pointer %s:\nbucket %zu pin %i prio %i gen %i last_gc %i mark %llu gc_gen %i",
		  buf, PTR_BUCKET_NR(b->c, k, ptr), atomic_read(&g->pin),
		  g->prio, g->gen, g->last_gc, GC_MARK(g), g->gc_gen);
	return true;
}

bool bch_extent_bad(struct btree_keys *bk, const struct bkey *k)
{
	struct btree *b = container_of(bk, struct btree, keys);
	struct bucket *g;
	unsigned i, stale;

	if (KEY_DELETED(k) ||
	    !KEY_PTRS(k) ||
	    bch_extent_invalid(bk, k))
		return true;

	for (i = 0; i < KEY_PTRS(k); i++)
		if (!ptr_available(b->c, k, i))
			return true;

	if (!expensive_debug_checks(b->c) && !KEY_CACHED(k))
		return false;

	for (i = 0; i < KEY_PTRS(k); i++) {
		g = PTR_BUCKET(b->c, k, i);
		stale = ptr_stale(b->c, k, i);

		btree_bug_on(stale > 96, b,
			     "key too stale: %i, need_gc %u",
			     stale, b->c->need_gc);

		btree_bug_on(stale && !KEY_CACHED(k) && KEY_SIZE(k),
			     b, "stale dirty pointer");

		if (stale)
			return true;

		if (expensive_debug_checks(b->c) &&
		    bch_extent_bad_expensive(b, k, i))
			return true;
	}

	return false;
}

static uint64_t merge_chksums(struct bkey *l, struct bkey *r)
{
	return (l->ptr[KEY_PTRS(l)] + r->ptr[KEY_PTRS(r)]) &
		~((uint64_t)1 << 63);
}

bool bch_extent_merge(struct btree_keys *bk, struct bkey *l, struct bkey *r)
{
	struct btree *b = container_of(bk, struct btree, keys);
	unsigned i;

	if (key_merging_disabled(b->c))
		return false;

	if (KEY_PTRS(l) != KEY_PTRS(r) ||
	    KEY_DELETED(l) != KEY_DELETED(r) ||
	    KEY_CACHED(l) != KEY_CACHED(r) ||
	    bkey_cmp(l, &START_KEY(r)))
		return false;

	for (i = 0; i < KEY_PTRS(l); i++)
		if (l->ptr[i] + PTR(0, KEY_SIZE(l), 0) != r->ptr[i] ||
		    PTR_BUCKET_NR(b->c, l, i) != PTR_BUCKET_NR(b->c, r, i))
			return false;

	/* Keys with no pointers aren't restricted to one bucket and could
	 * overflow KEY_SIZE
	 */
	if (KEY_SIZE(l) + KEY_SIZE(r) > USHRT_MAX) {
		SET_KEY_OFFSET(l, KEY_OFFSET(l) + USHRT_MAX - KEY_SIZE(l));
		SET_KEY_SIZE(l, USHRT_MAX);

		bch_cut_front(l, r);
		return false;
	}

	if (KEY_CSUM(l)) {
		if (KEY_CSUM(r))
			l->ptr[KEY_PTRS(l)] = merge_chksums(l, r);
		else
			SET_KEY_CSUM(l, 0);
	}

	SET_KEY_OFFSET(l, KEY_OFFSET(l) + KEY_SIZE(r));
	SET_KEY_SIZE(l, KEY_SIZE(l) + KEY_SIZE(r));

	return true;
}