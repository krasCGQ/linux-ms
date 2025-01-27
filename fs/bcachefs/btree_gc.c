// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 Kent Overstreet <kent.overstreet@gmail.com>
 * Copyright (C) 2014 Datera Inc.
 */

#include "bcachefs.h"
#include "alloc_background.h"
#include "alloc_foreground.h"
#include "bkey_methods.h"
#include "bkey_buf.h"
#include "btree_locking.h"
#include "btree_update_interior.h"
#include "btree_io.h"
#include "btree_gc.h"
#include "buckets.h"
#include "clock.h"
#include "debug.h"
#include "ec.h"
#include "error.h"
#include "extents.h"
#include "journal.h"
#include "keylist.h"
#include "move.h"
#include "recovery.h"
#include "replicas.h"
#include "super-io.h"

#include <linux/slab.h>
#include <linux/bitops.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <trace/events/bcachefs.h>

static inline void __gc_pos_set(struct bch_fs *c, struct gc_pos new_pos)
{
	preempt_disable();
	write_seqcount_begin(&c->gc_pos_lock);
	c->gc_pos = new_pos;
	write_seqcount_end(&c->gc_pos_lock);
	preempt_enable();
}

static inline void gc_pos_set(struct bch_fs *c, struct gc_pos new_pos)
{
	BUG_ON(gc_pos_cmp(new_pos, c->gc_pos) <= 0);
	__gc_pos_set(c, new_pos);
}

/*
 * Missing: if an interior btree node is empty, we need to do something -
 * perhaps just kill it
 */
static int bch2_gc_check_topology(struct bch_fs *c,
				  struct btree *b,
				  struct bkey_buf *prev,
				  struct bkey_buf cur,
				  bool is_last)
{
	struct bpos node_start	= b->data->min_key;
	struct bpos node_end	= b->data->max_key;
	struct bpos expected_start = bkey_deleted(&prev->k->k)
		? node_start
		: bkey_successor(prev->k->k.p);
	char buf1[200], buf2[200];
	bool update_min = false;
	bool update_max = false;
	int ret = 0;

	if (cur.k->k.type == KEY_TYPE_btree_ptr_v2) {
		struct bkey_i_btree_ptr_v2 *bp = bkey_i_to_btree_ptr_v2(cur.k);

		if (bkey_deleted(&prev->k->k))
			scnprintf(buf1, sizeof(buf1), "start of node: %llu:%llu",
				  node_start.inode,
				  node_start.offset);
		else
			bch2_bkey_val_to_text(&PBUF(buf1), c, bkey_i_to_s_c(prev->k));

		if (fsck_err_on(bkey_cmp(expected_start, bp->v.min_key), c,
				"btree node with incorrect min_key at btree %s level %u:\n"
				"  prev %s\n"
				"  cur %s",
				bch2_btree_ids[b->c.btree_id], b->c.level,
				buf1,
				(bch2_bkey_val_to_text(&PBUF(buf2), c, bkey_i_to_s_c(cur.k)), buf2)))
			update_min = true;
	}

	if (fsck_err_on(is_last &&
			bkey_cmp(cur.k->k.p, node_end), c,
			"btree node with incorrect max_key at btree %s level %u:\n"
			"  %s\n"
			"  expected %s",
			bch2_btree_ids[b->c.btree_id], b->c.level,
			(bch2_bkey_val_to_text(&PBUF(buf1), c, bkey_i_to_s_c(cur.k)), buf1),
			(bch2_bpos_to_text(&PBUF(buf2), node_end), buf2)))
		update_max = true;

	bch2_bkey_buf_copy(prev, c, cur.k);

	if (update_min || update_max) {
		struct bkey_i *new;
		struct bkey_i_btree_ptr_v2 *bp = NULL;
		struct btree *n;

		if (update_max) {
			ret = bch2_journal_key_delete(c, b->c.btree_id,
						      b->c.level, cur.k->k.p);
			if (ret)
				return ret;
		}

		new = kmalloc(bkey_bytes(&cur.k->k), GFP_KERNEL);
		if (!new) {
			bch_err(c, "%s: error allocating new key", __func__);
			return -ENOMEM;
		}

		bkey_copy(new, cur.k);

		if (new->k.type == KEY_TYPE_btree_ptr_v2)
			bp = bkey_i_to_btree_ptr_v2(new);

		if (update_min)
			bp->v.min_key = expected_start;
		if (update_max)
			new->k.p = node_end;
		if (bp)
			SET_BTREE_PTR_RANGE_UPDATED(&bp->v, true);

		ret = bch2_journal_key_insert(c, b->c.btree_id, b->c.level, new);
		if (ret) {
			kfree(new);
			return ret;
		}

		n = bch2_btree_node_get_noiter(c, cur.k, b->c.btree_id,
					       b->c.level - 1, true);
		if (n) {
			mutex_lock(&c->btree_cache.lock);
			bch2_btree_node_hash_remove(&c->btree_cache, n);

			bkey_copy(&n->key, new);
			if (update_min)
				n->data->min_key = expected_start;
			if (update_max)
				n->data->max_key = node_end;

			ret = __bch2_btree_node_hash_insert(&c->btree_cache, n);
			BUG_ON(ret);
			mutex_unlock(&c->btree_cache.lock);
			six_unlock_read(&n->c.lock);
		}
	}
fsck_err:
	return ret;
}

static int bch2_check_fix_ptrs(struct bch_fs *c, enum btree_id btree_id,
			       unsigned level, bool is_root,
			       struct bkey_s_c *k)
{
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(*k);
	const union bch_extent_entry *entry;
	struct extent_ptr_decoded p;
	bool do_update = false;
	int ret = 0;

	bkey_for_each_ptr_decode(k->k, ptrs, p, entry) {
		struct bch_dev *ca = bch_dev_bkey_exists(c, p.ptr.dev);
		struct bucket *g = PTR_BUCKET(ca, &p.ptr, true);
		struct bucket *g2 = PTR_BUCKET(ca, &p.ptr, false);

		if (fsck_err_on(!g->gen_valid, c,
				"bucket %u:%zu data type %s ptr gen %u missing in alloc btree",
				p.ptr.dev, PTR_BUCKET_NR(ca, &p.ptr),
				bch2_data_types[ptr_data_type(k->k, &p.ptr)],
				p.ptr.gen)) {
			if (p.ptr.cached) {
				g2->_mark.gen	= g->_mark.gen		= p.ptr.gen;
				g2->gen_valid	= g->gen_valid		= true;
				set_bit(BCH_FS_NEED_ALLOC_WRITE, &c->flags);
			} else {
				do_update = true;
			}
		}

		if (fsck_err_on(gen_cmp(p.ptr.gen, g->mark.gen) > 0, c,
				"bucket %u:%zu data type %s ptr gen in the future: %u > %u",
				p.ptr.dev, PTR_BUCKET_NR(ca, &p.ptr),
				bch2_data_types[ptr_data_type(k->k, &p.ptr)],
				p.ptr.gen, g->mark.gen)) {
			if (p.ptr.cached) {
				g2->_mark.gen	= g->_mark.gen	= p.ptr.gen;
				g2->gen_valid	= g->gen_valid	= true;
				g2->_mark.data_type		= 0;
				g2->_mark.dirty_sectors		= 0;
				g2->_mark.cached_sectors	= 0;
				set_bit(BCH_FS_NEED_ANOTHER_GC, &c->flags);
				set_bit(BCH_FS_NEED_ALLOC_WRITE, &c->flags);
			} else {
				do_update = true;
			}
		}

		if (fsck_err_on(!p.ptr.cached &&
				gen_cmp(p.ptr.gen, g->mark.gen) < 0, c,
				"bucket %u:%zu data type %s stale dirty ptr: %u < %u",
				p.ptr.dev, PTR_BUCKET_NR(ca, &p.ptr),
				bch2_data_types[ptr_data_type(k->k, &p.ptr)],
				p.ptr.gen, g->mark.gen))
			do_update = true;

		if (p.has_ec) {
			struct stripe *m = genradix_ptr(&c->stripes[true], p.ec.idx);

			if (fsck_err_on(!m || !m->alive, c,
					"pointer to nonexistent stripe %llu",
					(u64) p.ec.idx))
				do_update = true;
		}
	}

	if (do_update) {
		struct bkey_ptrs ptrs;
		union bch_extent_entry *entry;
		struct bch_extent_ptr *ptr;
		struct bkey_i *new;

		if (is_root) {
			bch_err(c, "cannot update btree roots yet");
			return -EINVAL;
		}

		new = kmalloc(bkey_bytes(k->k), GFP_KERNEL);
		if (!new) {
			bch_err(c, "%s: error allocating new key", __func__);
			return -ENOMEM;
		}

		bkey_reassemble(new, *k);

		bch2_bkey_drop_ptrs(bkey_i_to_s(new), ptr, ({
			struct bch_dev *ca = bch_dev_bkey_exists(c, ptr->dev);
			struct bucket *g = PTR_BUCKET(ca, ptr, true);

			(ptr->cached &&
			 (!g->gen_valid || gen_cmp(ptr->gen, g->mark.gen) > 0)) ||
			(!ptr->cached &&
			 gen_cmp(ptr->gen, g->mark.gen) < 0);
		}));
again:
		ptrs = bch2_bkey_ptrs(bkey_i_to_s(new));
		bkey_extent_entry_for_each(ptrs, entry) {
			if (extent_entry_type(entry) == BCH_EXTENT_ENTRY_stripe_ptr) {
				struct stripe *m = genradix_ptr(&c->stripes[true],
								entry->stripe_ptr.idx);

				if (!m || !m->alive) {
					bch2_bkey_extent_entry_drop(new, entry);
					goto again;
				}
			}
		}

		ret = bch2_journal_key_insert(c, btree_id, level, new);
		if (ret)
			kfree(new);
		else
			*k = bkey_i_to_s_c(new);
	}
fsck_err:
	return ret;
}

/* marking of btree keys/nodes: */

static int bch2_gc_mark_key(struct bch_fs *c, enum btree_id btree_id,
			    unsigned level, bool is_root,
			    struct bkey_s_c k,
			    u8 *max_stale, bool initial)
{
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	const struct bch_extent_ptr *ptr;
	unsigned flags =
		BTREE_TRIGGER_GC|
		(initial ? BTREE_TRIGGER_NOATOMIC : 0);
	int ret = 0;

	if (initial) {
		BUG_ON(bch2_journal_seq_verify &&
		       k.k->version.lo > journal_cur_seq(&c->journal));

		if (fsck_err_on(k.k->version.lo > atomic64_read(&c->key_version), c,
				"key version number higher than recorded: %llu > %llu",
				k.k->version.lo,
				atomic64_read(&c->key_version)))
			atomic64_set(&c->key_version, k.k->version.lo);

		if (test_bit(BCH_FS_REBUILD_REPLICAS, &c->flags) ||
		    fsck_err_on(!bch2_bkey_replicas_marked(c, k), c,
				"superblock not marked as containing replicas (type %u)",
				k.k->type)) {
			ret = bch2_mark_bkey_replicas(c, k);
			if (ret) {
				bch_err(c, "error marking bkey replicas: %i", ret);
				goto err;
			}
		}

		ret = bch2_check_fix_ptrs(c, btree_id, level, is_root, &k);
	}

	bkey_for_each_ptr(ptrs, ptr) {
		struct bch_dev *ca = bch_dev_bkey_exists(c, ptr->dev);
		struct bucket *g = PTR_BUCKET(ca, ptr, true);

		if (gen_after(g->oldest_gen, ptr->gen))
			g->oldest_gen = ptr->gen;

		*max_stale = max(*max_stale, ptr_stale(ca, ptr));
	}

	bch2_mark_key(c, k, 0, k.k->size, NULL, 0, flags);
fsck_err:
err:
	if (ret)
		bch_err(c, "%s: ret %i", __func__, ret);
	return ret;
}

static int btree_gc_mark_node(struct bch_fs *c, struct btree *b, u8 *max_stale,
			      bool initial)
{
	struct btree_node_iter iter;
	struct bkey unpacked;
	struct bkey_s_c k;
	struct bkey_buf prev, cur;
	int ret = 0;

	*max_stale = 0;

	if (!btree_node_type_needs_gc(btree_node_type(b)))
		return 0;

	bch2_btree_node_iter_init_from_start(&iter, b);
	bch2_bkey_buf_init(&prev);
	bch2_bkey_buf_init(&cur);
	bkey_init(&prev.k->k);

	while ((k = bch2_btree_node_iter_peek_unpack(&iter, b, &unpacked)).k) {
		bch2_bkey_debugcheck(c, b, k);

		ret = bch2_gc_mark_key(c, b->c.btree_id, b->c.level, false,
				       k, max_stale, initial);
		if (ret)
			break;

		bch2_btree_node_iter_advance(&iter, b);

		if (b->c.level) {
			bch2_bkey_buf_reassemble(&cur, c, k);

			ret = bch2_gc_check_topology(c, b, &prev, cur,
					bch2_btree_node_iter_end(&iter));
			if (ret)
				break;
		}
	}

	bch2_bkey_buf_exit(&cur, c);
	bch2_bkey_buf_exit(&prev, c);
	return ret;
}

static int bch2_gc_btree(struct bch_fs *c, enum btree_id btree_id,
			 bool initial)
{
	struct btree_trans trans;
	struct btree_iter *iter;
	struct btree *b;
	unsigned depth = bch2_expensive_debug_checks				? 0
		: !btree_node_type_needs_gc((enum btree_node_type)btree_id)	? 1
		: 0;
	u8 max_stale = 0;
	int ret = 0;

	bch2_trans_init(&trans, c, 0, 0);

	gc_pos_set(c, gc_pos_btree(btree_id, POS_MIN, 0));

	__for_each_btree_node(&trans, iter, btree_id, POS_MIN,
			      0, depth, BTREE_ITER_PREFETCH, b) {
		bch2_verify_btree_nr_keys(b);

		gc_pos_set(c, gc_pos_btree_node(b));

		ret = btree_gc_mark_node(c, b, &max_stale, initial);
		if (ret)
			break;

		if (!initial) {
			if (max_stale > 64)
				bch2_btree_node_rewrite(c, iter,
						b->data->keys.seq,
						BTREE_INSERT_NOWAIT|
						BTREE_INSERT_GC_LOCK_HELD);
			else if (!bch2_btree_gc_rewrite_disabled &&
				 (bch2_btree_gc_always_rewrite || max_stale > 16))
				bch2_btree_node_rewrite(c, iter,
						b->data->keys.seq,
						BTREE_INSERT_NOWAIT|
						BTREE_INSERT_GC_LOCK_HELD);
		}

		bch2_trans_cond_resched(&trans);
	}
	ret = bch2_trans_exit(&trans) ?: ret;
	if (ret)
		return ret;

	mutex_lock(&c->btree_root_lock);
	b = c->btree_roots[btree_id].b;
	if (!btree_node_fake(b))
		ret = bch2_gc_mark_key(c, b->c.btree_id, b->c.level, true,
				       bkey_i_to_s_c(&b->key),
				       &max_stale, initial);
	gc_pos_set(c, gc_pos_btree_root(b->c.btree_id));
	mutex_unlock(&c->btree_root_lock);

	return ret;
}

static int bch2_gc_btree_init_recurse(struct bch_fs *c, struct btree *b,
				      unsigned target_depth)
{
	struct btree_and_journal_iter iter;
	struct bkey_s_c k;
	struct bkey_buf cur, prev;
	u8 max_stale = 0;
	int ret = 0;

	bch2_btree_and_journal_iter_init_node_iter(&iter, c, b);
	bch2_bkey_buf_init(&prev);
	bch2_bkey_buf_init(&cur);
	bkey_init(&prev.k->k);

	while ((k = bch2_btree_and_journal_iter_peek(&iter)).k) {
		bch2_bkey_debugcheck(c, b, k);

		BUG_ON(bkey_cmp(k.k->p, b->data->min_key) < 0);
		BUG_ON(bkey_cmp(k.k->p, b->data->max_key) > 0);

		ret = bch2_gc_mark_key(c, b->c.btree_id, b->c.level, false,
				       k, &max_stale, true);
		if (ret) {
			bch_err(c, "%s: error %i from bch2_gc_mark_key", __func__, ret);
			break;
		}

		if (b->c.level) {
			bch2_bkey_buf_reassemble(&cur, c, k);
			k = bkey_i_to_s_c(cur.k);

			bch2_btree_and_journal_iter_advance(&iter);

			ret = bch2_gc_check_topology(c, b,
					&prev, cur,
					!bch2_btree_and_journal_iter_peek(&iter).k);
			if (ret)
				break;
		} else {
			bch2_btree_and_journal_iter_advance(&iter);
		}
	}

	if (b->c.level > target_depth) {
		bch2_btree_and_journal_iter_exit(&iter);
		bch2_btree_and_journal_iter_init_node_iter(&iter, c, b);

		while ((k = bch2_btree_and_journal_iter_peek(&iter)).k) {
			struct btree *child;

			bch2_bkey_buf_reassemble(&cur, c, k);
			bch2_btree_and_journal_iter_advance(&iter);

			child = bch2_btree_node_get_noiter(c, cur.k,
						b->c.btree_id, b->c.level - 1,
						false);
			ret = PTR_ERR_OR_ZERO(child);

			if (fsck_err_on(ret == -EIO, c,
					"unreadable btree node")) {
				ret = bch2_journal_key_delete(c, b->c.btree_id,
							      b->c.level, cur.k->k.p);
				if (ret)
					return ret;

				set_bit(BCH_FS_NEED_ANOTHER_GC, &c->flags);
				continue;
			}

			if (ret) {
				bch_err(c, "%s: error %i getting btree node",
					__func__, ret);
				break;
			}

			ret = bch2_gc_btree_init_recurse(c, child,
							 target_depth);
			six_unlock_read(&child->c.lock);

			if (ret)
				break;
		}
	}
fsck_err:
	bch2_bkey_buf_exit(&cur, c);
	bch2_bkey_buf_exit(&prev, c);
	bch2_btree_and_journal_iter_exit(&iter);
	return ret;
}

static int bch2_gc_btree_init(struct bch_fs *c,
			      enum btree_id btree_id)
{
	struct btree *b;
	unsigned target_depth = bch2_expensive_debug_checks			? 0
		: !btree_node_type_needs_gc((enum btree_node_type)btree_id)	? 1
		: 0;
	u8 max_stale = 0;
	int ret = 0;

	b = c->btree_roots[btree_id].b;

	if (btree_node_fake(b))
		return 0;

	six_lock_read(&b->c.lock, NULL, NULL);
	if (fsck_err_on(bkey_cmp(b->data->min_key, POS_MIN), c,
			"btree root with incorrect min_key: %llu:%llu",
			b->data->min_key.inode,
			b->data->min_key.offset)) {
		BUG();
	}

	if (fsck_err_on(bkey_cmp(b->data->max_key, POS_MAX), c,
			"btree root with incorrect min_key: %llu:%llu",
			b->data->max_key.inode,
			b->data->max_key.offset)) {
		BUG();
	}

	if (b->c.level >= target_depth)
		ret = bch2_gc_btree_init_recurse(c, b, target_depth);

	if (!ret)
		ret = bch2_gc_mark_key(c, b->c.btree_id, b->c.level, true,
				       bkey_i_to_s_c(&b->key),
				       &max_stale, true);
fsck_err:
	six_unlock_read(&b->c.lock);

	if (ret)
		bch_err(c, "%s: ret %i", __func__, ret);
	return ret;
}

static inline int btree_id_gc_phase_cmp(enum btree_id l, enum btree_id r)
{
	return  (int) btree_id_to_gc_phase(l) -
		(int) btree_id_to_gc_phase(r);
}

static int bch2_gc_btrees(struct bch_fs *c, bool initial)
{
	enum btree_id ids[BTREE_ID_NR];
	unsigned i;

	for (i = 0; i < BTREE_ID_NR; i++)
		ids[i] = i;
	bubble_sort(ids, BTREE_ID_NR, btree_id_gc_phase_cmp);

	for (i = 0; i < BTREE_ID_NR; i++) {
		enum btree_id id = ids[i];
		int ret = initial
			? bch2_gc_btree_init(c, id)
			: bch2_gc_btree(c, id, initial);
		if (ret) {
			bch_err(c, "%s: ret %i", __func__, ret);
			return ret;
		}
	}

	return 0;
}

static void mark_metadata_sectors(struct bch_fs *c, struct bch_dev *ca,
				  u64 start, u64 end,
				  enum bch_data_type type,
				  unsigned flags)
{
	u64 b = sector_to_bucket(ca, start);

	do {
		unsigned sectors =
			min_t(u64, bucket_to_sector(ca, b + 1), end) - start;

		bch2_mark_metadata_bucket(c, ca, b, type, sectors,
					  gc_phase(GC_PHASE_SB), flags);
		b++;
		start += sectors;
	} while (start < end);
}

void bch2_mark_dev_superblock(struct bch_fs *c, struct bch_dev *ca,
			      unsigned flags)
{
	struct bch_sb_layout *layout = &ca->disk_sb.sb->layout;
	unsigned i;
	u64 b;

	/*
	 * This conditional is kind of gross, but we may be called from the
	 * device add path, before the new device has actually been added to the
	 * running filesystem:
	 */
	if (c) {
		lockdep_assert_held(&c->sb_lock);
		percpu_down_read(&c->mark_lock);
	}

	for (i = 0; i < layout->nr_superblocks; i++) {
		u64 offset = le64_to_cpu(layout->sb_offset[i]);

		if (offset == BCH_SB_SECTOR)
			mark_metadata_sectors(c, ca, 0, BCH_SB_SECTOR,
					      BCH_DATA_sb, flags);

		mark_metadata_sectors(c, ca, offset,
				      offset + (1 << layout->sb_max_size_bits),
				      BCH_DATA_sb, flags);
	}

	for (i = 0; i < ca->journal.nr; i++) {
		b = ca->journal.buckets[i];
		bch2_mark_metadata_bucket(c, ca, b, BCH_DATA_journal,
					  ca->mi.bucket_size,
					  gc_phase(GC_PHASE_SB), flags);
	}

	if (c)
		percpu_up_read(&c->mark_lock);
}

static void bch2_mark_superblocks(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;

	mutex_lock(&c->sb_lock);
	gc_pos_set(c, gc_phase(GC_PHASE_SB));

	for_each_online_member(ca, c, i)
		bch2_mark_dev_superblock(c, ca, BTREE_TRIGGER_GC);
	mutex_unlock(&c->sb_lock);
}

#if 0
/* Also see bch2_pending_btree_node_free_insert_done() */
static void bch2_mark_pending_btree_node_frees(struct bch_fs *c)
{
	struct btree_update *as;
	struct pending_btree_node_free *d;

	mutex_lock(&c->btree_interior_update_lock);
	gc_pos_set(c, gc_phase(GC_PHASE_PENDING_DELETE));

	for_each_pending_btree_node_free(c, as, d)
		if (d->index_update_done)
			bch2_mark_key(c, bkey_i_to_s_c(&d->key),
				      0, 0, NULL, 0,
				      BTREE_TRIGGER_GC);

	mutex_unlock(&c->btree_interior_update_lock);
}
#endif

static void bch2_mark_allocator_buckets(struct bch_fs *c)
{
	struct bch_dev *ca;
	struct open_bucket *ob;
	size_t i, j, iter;
	unsigned ci;

	percpu_down_read(&c->mark_lock);

	spin_lock(&c->freelist_lock);
	gc_pos_set(c, gc_pos_alloc(c, NULL));

	for_each_member_device(ca, c, ci) {
		fifo_for_each_entry(i, &ca->free_inc, iter)
			bch2_mark_alloc_bucket(c, ca, i, true,
					       gc_pos_alloc(c, NULL),
					       BTREE_TRIGGER_GC);



		for (j = 0; j < RESERVE_NR; j++)
			fifo_for_each_entry(i, &ca->free[j], iter)
				bch2_mark_alloc_bucket(c, ca, i, true,
						       gc_pos_alloc(c, NULL),
						       BTREE_TRIGGER_GC);
	}

	spin_unlock(&c->freelist_lock);

	for (ob = c->open_buckets;
	     ob < c->open_buckets + ARRAY_SIZE(c->open_buckets);
	     ob++) {
		spin_lock(&ob->lock);
		if (ob->valid) {
			gc_pos_set(c, gc_pos_alloc(c, ob));
			ca = bch_dev_bkey_exists(c, ob->ptr.dev);
			bch2_mark_alloc_bucket(c, ca, PTR_BUCKET_NR(ca, &ob->ptr), true,
					       gc_pos_alloc(c, ob),
					       BTREE_TRIGGER_GC);
		}
		spin_unlock(&ob->lock);
	}

	percpu_up_read(&c->mark_lock);
}

static void bch2_gc_free(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;

	genradix_free(&c->stripes[1]);

	for_each_member_device(ca, c, i) {
		kvpfree(rcu_dereference_protected(ca->buckets[1], 1),
			sizeof(struct bucket_array) +
			ca->mi.nbuckets * sizeof(struct bucket));
		ca->buckets[1] = NULL;

		free_percpu(ca->usage_gc);
		ca->usage_gc = NULL;
	}

	free_percpu(c->usage_gc);
	c->usage_gc = NULL;
}

static int bch2_gc_done(struct bch_fs *c,
			bool initial)
{
	struct bch_dev *ca;
	bool verify = (!initial ||
		       (c->sb.compat & (1ULL << BCH_COMPAT_FEAT_ALLOC_INFO)));
	unsigned i, dev;
	int ret = 0;

#define copy_field(_f, _msg, ...)					\
	if (dst->_f != src->_f) {					\
		if (verify)						\
			fsck_err(c, _msg ": got %llu, should be %llu"	\
				, ##__VA_ARGS__, dst->_f, src->_f);	\
		dst->_f = src->_f;					\
		set_bit(BCH_FS_NEED_ALLOC_WRITE, &c->flags);		\
	}
#define copy_stripe_field(_f, _msg, ...)				\
	if (dst->_f != src->_f) {					\
		if (verify)						\
			fsck_err(c, "stripe %zu has wrong "_msg		\
				": got %u, should be %u",		\
				iter.pos, ##__VA_ARGS__,		\
				dst->_f, src->_f);			\
		dst->_f = src->_f;					\
		set_bit(BCH_FS_NEED_ALLOC_WRITE, &c->flags);		\
	}
#define copy_bucket_field(_f)						\
	if (dst->b[b].mark._f != src->b[b].mark._f) {			\
		if (verify)						\
			fsck_err(c, "bucket %u:%zu gen %u data type %s has wrong " #_f	\
				": got %u, should be %u", i, b,		\
				dst->b[b].mark.gen,			\
				bch2_data_types[dst->b[b].mark.data_type],\
				dst->b[b].mark._f, src->b[b].mark._f);	\
		dst->b[b]._mark._f = src->b[b].mark._f;			\
		set_bit(BCH_FS_NEED_ALLOC_WRITE, &c->flags);		\
	}
#define copy_dev_field(_f, _msg, ...)					\
	copy_field(_f, "dev %u has wrong " _msg, i, ##__VA_ARGS__)
#define copy_fs_field(_f, _msg, ...)					\
	copy_field(_f, "fs has wrong " _msg, ##__VA_ARGS__)

	{
		struct genradix_iter iter = genradix_iter_init(&c->stripes[1], 0);
		struct stripe *dst, *src;

		while ((src = genradix_iter_peek(&iter, &c->stripes[1]))) {
			dst = genradix_ptr_alloc(&c->stripes[0], iter.pos, GFP_KERNEL);

			if (dst->alive		!= src->alive ||
			    dst->sectors	!= src->sectors ||
			    dst->algorithm	!= src->algorithm ||
			    dst->nr_blocks	!= src->nr_blocks ||
			    dst->nr_redundant	!= src->nr_redundant) {
				bch_err(c, "unexpected stripe inconsistency at bch2_gc_done, confused");
				ret = -EINVAL;
				goto fsck_err;
			}

			for (i = 0; i < ARRAY_SIZE(dst->block_sectors); i++)
				copy_stripe_field(block_sectors[i],
						  "block_sectors[%u]", i);

			dst->blocks_nonempty = 0;
			for (i = 0; i < dst->nr_blocks; i++)
				dst->blocks_nonempty += dst->block_sectors[i] != 0;

			genradix_iter_advance(&iter, &c->stripes[1]);
		}
	}

	for (i = 0; i < ARRAY_SIZE(c->usage); i++)
		bch2_fs_usage_acc_to_base(c, i);

	for_each_member_device(ca, c, dev) {
		struct bucket_array *dst = __bucket_array(ca, 0);
		struct bucket_array *src = __bucket_array(ca, 1);
		size_t b;

		for (b = 0; b < src->nbuckets; b++) {
			copy_bucket_field(gen);
			copy_bucket_field(data_type);
			copy_bucket_field(owned_by_allocator);
			copy_bucket_field(stripe);
			copy_bucket_field(dirty_sectors);
			copy_bucket_field(cached_sectors);

			dst->b[b].oldest_gen = src->b[b].oldest_gen;
		}

		{
			struct bch_dev_usage *dst = ca->usage_base;
			struct bch_dev_usage *src = (void *)
				bch2_acc_percpu_u64s((void *) ca->usage_gc,
						     dev_usage_u64s());

			copy_dev_field(buckets_ec,		"buckets_ec");
			copy_dev_field(buckets_unavailable,	"buckets_unavailable");

			for (i = 0; i < BCH_DATA_NR; i++) {
				copy_dev_field(d[i].buckets,	"%s buckets", bch2_data_types[i]);
				copy_dev_field(d[i].sectors,	"%s sectors", bch2_data_types[i]);
				copy_dev_field(d[i].fragmented,	"%s fragmented", bch2_data_types[i]);
			}
		}
	};

	{
		unsigned nr = fs_usage_u64s(c);
		struct bch_fs_usage *dst = c->usage_base;
		struct bch_fs_usage *src = (void *)
			bch2_acc_percpu_u64s((void *) c->usage_gc, nr);

		copy_fs_field(hidden,		"hidden");
		copy_fs_field(btree,		"btree");
		copy_fs_field(data,	"data");
		copy_fs_field(cached,	"cached");
		copy_fs_field(reserved,	"reserved");
		copy_fs_field(nr_inodes,"nr_inodes");

		for (i = 0; i < BCH_REPLICAS_MAX; i++)
			copy_fs_field(persistent_reserved[i],
				      "persistent_reserved[%i]", i);

		for (i = 0; i < c->replicas.nr; i++) {
			struct bch_replicas_entry *e =
				cpu_replicas_entry(&c->replicas, i);
			char buf[80];

			bch2_replicas_entry_to_text(&PBUF(buf), e);

			copy_fs_field(replicas[i], "%s", buf);
		}
	}

#undef copy_fs_field
#undef copy_dev_field
#undef copy_bucket_field
#undef copy_stripe_field
#undef copy_field
fsck_err:
	if (ret)
		bch_err(c, "%s: ret %i", __func__, ret);
	return ret;
}

static int bch2_gc_start(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;
	int ret;

	BUG_ON(c->usage_gc);

	c->usage_gc = __alloc_percpu_gfp(fs_usage_u64s(c) * sizeof(u64),
					 sizeof(u64), GFP_KERNEL);
	if (!c->usage_gc) {
		bch_err(c, "error allocating c->usage_gc");
		return -ENOMEM;
	}

	for_each_member_device(ca, c, i) {
		BUG_ON(ca->buckets[1]);
		BUG_ON(ca->usage_gc);

		ca->buckets[1] = kvpmalloc(sizeof(struct bucket_array) +
				ca->mi.nbuckets * sizeof(struct bucket),
				GFP_KERNEL|__GFP_ZERO);
		if (!ca->buckets[1]) {
			percpu_ref_put(&ca->ref);
			bch_err(c, "error allocating ca->buckets[gc]");
			return -ENOMEM;
		}

		ca->usage_gc = alloc_percpu(struct bch_dev_usage);
		if (!ca->usage_gc) {
			bch_err(c, "error allocating ca->usage_gc");
			percpu_ref_put(&ca->ref);
			return -ENOMEM;
		}
	}

	ret = bch2_ec_mem_alloc(c, true);
	if (ret) {
		bch_err(c, "error allocating ec gc mem");
		return ret;
	}

	percpu_down_write(&c->mark_lock);

	/*
	 * indicate to stripe code that we need to allocate for the gc stripes
	 * radix tree, too
	 */
	gc_pos_set(c, gc_phase(GC_PHASE_START));

	for_each_member_device(ca, c, i) {
		struct bucket_array *dst = __bucket_array(ca, 1);
		struct bucket_array *src = __bucket_array(ca, 0);
		size_t b;

		dst->first_bucket	= src->first_bucket;
		dst->nbuckets		= src->nbuckets;

		for (b = 0; b < src->nbuckets; b++) {
			struct bucket *d = &dst->b[b];
			struct bucket *s = &src->b[b];

			d->_mark.gen = dst->b[b].oldest_gen = s->mark.gen;
			d->gen_valid = s->gen_valid;
		}
	};

	percpu_up_write(&c->mark_lock);

	return 0;
}

/**
 * bch2_gc - walk _all_ references to buckets, and recompute them:
 *
 * Order matters here:
 *  - Concurrent GC relies on the fact that we have a total ordering for
 *    everything that GC walks - see  gc_will_visit_node(),
 *    gc_will_visit_root()
 *
 *  - also, references move around in the course of index updates and
 *    various other crap: everything needs to agree on the ordering
 *    references are allowed to move around in - e.g., we're allowed to
 *    start with a reference owned by an open_bucket (the allocator) and
 *    move it to the btree, but not the reverse.
 *
 *    This is necessary to ensure that gc doesn't miss references that
 *    move around - if references move backwards in the ordering GC
 *    uses, GC could skip past them
 */
int bch2_gc(struct bch_fs *c, bool initial)
{
	struct bch_dev *ca;
	u64 start_time = local_clock();
	unsigned i, iter = 0;
	int ret;

	lockdep_assert_held(&c->state_lock);
	trace_gc_start(c);

	down_write(&c->gc_lock);

	/* flush interior btree updates: */
	closure_wait_event(&c->btree_interior_update_wait,
			   !bch2_btree_interior_updates_nr_pending(c));
again:
	ret = bch2_gc_start(c);
	if (ret)
		goto out;

	bch2_mark_superblocks(c);

	ret = bch2_gc_btrees(c, initial);
	if (ret)
		goto out;

#if 0
	bch2_mark_pending_btree_node_frees(c);
#endif
	bch2_mark_allocator_buckets(c);

	c->gc_count++;

	if (test_bit(BCH_FS_NEED_ANOTHER_GC, &c->flags) ||
	    (!iter && bch2_test_restart_gc)) {
		/*
		 * XXX: make sure gens we fixed got saved
		 */
		if (iter++ <= 2) {
			bch_info(c, "Second GC pass needed, restarting:");
			clear_bit(BCH_FS_NEED_ANOTHER_GC, &c->flags);
			__gc_pos_set(c, gc_phase(GC_PHASE_NOT_RUNNING));

			percpu_down_write(&c->mark_lock);
			bch2_gc_free(c);
			percpu_up_write(&c->mark_lock);
			/* flush fsck errors, reset counters */
			bch2_flush_fsck_errs(c);

			goto again;
		}

		bch_info(c, "Unable to fix bucket gens, looping");
		ret = -EINVAL;
	}
out:
	if (!ret) {
		bch2_journal_block(&c->journal);

		percpu_down_write(&c->mark_lock);
		ret = bch2_gc_done(c, initial);

		bch2_journal_unblock(&c->journal);
	} else {
		percpu_down_write(&c->mark_lock);
	}

	/* Indicates that gc is no longer in progress: */
	__gc_pos_set(c, gc_phase(GC_PHASE_NOT_RUNNING));

	bch2_gc_free(c);
	percpu_up_write(&c->mark_lock);

	up_write(&c->gc_lock);

	trace_gc_end(c);
	bch2_time_stats_update(&c->times[BCH_TIME_btree_gc], start_time);

	/*
	 * Wake up allocator in case it was waiting for buckets
	 * because of not being able to inc gens
	 */
	for_each_member_device(ca, c, i)
		bch2_wake_allocator(ca);

	/*
	 * At startup, allocations can happen directly instead of via the
	 * allocator thread - issue wakeup in case they blocked on gc_lock:
	 */
	closure_wake_up(&c->freelist_wait);
	return ret;
}

static bool gc_btree_gens_key(struct bch_fs *c, struct bkey_s_c k)
{
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	const struct bch_extent_ptr *ptr;

	percpu_down_read(&c->mark_lock);
	bkey_for_each_ptr(ptrs, ptr) {
		struct bch_dev *ca = bch_dev_bkey_exists(c, ptr->dev);
		struct bucket *g = PTR_BUCKET(ca, ptr, false);

		if (gen_after(g->mark.gen, ptr->gen) > 16) {
			percpu_up_read(&c->mark_lock);
			return true;
		}
	}

	bkey_for_each_ptr(ptrs, ptr) {
		struct bch_dev *ca = bch_dev_bkey_exists(c, ptr->dev);
		struct bucket *g = PTR_BUCKET(ca, ptr, false);

		if (gen_after(g->gc_gen, ptr->gen))
			g->gc_gen = ptr->gen;
	}
	percpu_up_read(&c->mark_lock);

	return false;
}

/*
 * For recalculating oldest gen, we only need to walk keys in leaf nodes; btree
 * node pointers currently never have cached pointers that can become stale:
 */
static int bch2_gc_btree_gens(struct bch_fs *c, enum btree_id btree_id)
{
	struct btree_trans trans;
	struct btree_iter *iter;
	struct bkey_s_c k;
	struct bkey_buf sk;
	int ret = 0;

	bch2_bkey_buf_init(&sk);
	bch2_trans_init(&trans, c, 0, 0);

	iter = bch2_trans_get_iter(&trans, btree_id, POS_MIN,
				   BTREE_ITER_PREFETCH);

	while ((k = bch2_btree_iter_peek(iter)).k &&
	       !(ret = bkey_err(k))) {
		if (gc_btree_gens_key(c, k)) {
			bch2_bkey_buf_reassemble(&sk, c, k);
			bch2_extent_normalize(c, bkey_i_to_s(sk.k));

			bch2_btree_iter_set_pos(iter, bkey_start_pos(&sk.k->k));

			bch2_trans_update(&trans, iter, sk.k, 0);

			ret = bch2_trans_commit(&trans, NULL, NULL,
						BTREE_INSERT_NOFAIL);
			if (ret == -EINTR)
				continue;
			if (ret) {
				break;
			}
		}

		bch2_btree_iter_next(iter);
	}

	bch2_trans_exit(&trans);
	bch2_bkey_buf_exit(&sk, c);

	return ret;
}

int bch2_gc_gens(struct bch_fs *c)
{
	struct bch_dev *ca;
	struct bucket_array *buckets;
	struct bucket *g;
	unsigned i;
	int ret;

	/*
	 * Ideally we would be using state_lock and not gc_lock here, but that
	 * introduces a deadlock in the RO path - we currently take the state
	 * lock at the start of going RO, thus the gc thread may get stuck:
	 */
	down_read(&c->gc_lock);

	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		buckets = bucket_array(ca);

		for_each_bucket(g, buckets)
			g->gc_gen = g->mark.gen;
		up_read(&ca->bucket_lock);
	}

	for (i = 0; i < BTREE_ID_NR; i++)
		if (btree_node_type_needs_gc(i)) {
			ret = bch2_gc_btree_gens(c, i);
			if (ret) {
				bch_err(c, "error recalculating oldest_gen: %i", ret);
				goto err;
			}
		}

	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		buckets = bucket_array(ca);

		for_each_bucket(g, buckets)
			g->oldest_gen = g->gc_gen;
		up_read(&ca->bucket_lock);
	}

	c->gc_count++;
err:
	up_read(&c->gc_lock);
	return ret;
}

/* Btree coalescing */

static void recalc_packed_keys(struct btree *b)
{
	struct bset *i = btree_bset_first(b);
	struct bkey_packed *k;

	memset(&b->nr, 0, sizeof(b->nr));

	BUG_ON(b->nsets != 1);

	vstruct_for_each(i, k)
		btree_keys_account_key_add(&b->nr, 0, k);
}

static void bch2_coalesce_nodes(struct bch_fs *c, struct btree_iter *iter,
				struct btree *old_nodes[GC_MERGE_NODES])
{
	struct btree *parent = btree_node_parent(iter, old_nodes[0]);
	unsigned i, nr_old_nodes, nr_new_nodes, u64s = 0;
	unsigned blocks = btree_blocks(c) * 2 / 3;
	struct btree *new_nodes[GC_MERGE_NODES];
	struct btree_update *as;
	struct keylist keylist;
	struct bkey_format_state format_state;
	struct bkey_format new_format;

	memset(new_nodes, 0, sizeof(new_nodes));
	bch2_keylist_init(&keylist, NULL);

	/* Count keys that are not deleted */
	for (i = 0; i < GC_MERGE_NODES && old_nodes[i]; i++)
		u64s += old_nodes[i]->nr.live_u64s;

	nr_old_nodes = nr_new_nodes = i;

	/* Check if all keys in @old_nodes could fit in one fewer node */
	if (nr_old_nodes <= 1 ||
	    __vstruct_blocks(struct btree_node, c->block_bits,
			     DIV_ROUND_UP(u64s, nr_old_nodes - 1)) > blocks)
		return;

	/* Find a format that all keys in @old_nodes can pack into */
	bch2_bkey_format_init(&format_state);

	for (i = 0; i < nr_old_nodes; i++)
		__bch2_btree_calc_format(&format_state, old_nodes[i]);

	new_format = bch2_bkey_format_done(&format_state);

	/* Check if repacking would make any nodes too big to fit */
	for (i = 0; i < nr_old_nodes; i++)
		if (!bch2_btree_node_format_fits(c, old_nodes[i], &new_format)) {
			trace_btree_gc_coalesce_fail(c,
					BTREE_GC_COALESCE_FAIL_FORMAT_FITS);
			return;
		}

	if (bch2_keylist_realloc(&keylist, NULL, 0,
			BKEY_BTREE_PTR_U64s_MAX * nr_old_nodes)) {
		trace_btree_gc_coalesce_fail(c,
				BTREE_GC_COALESCE_FAIL_KEYLIST_REALLOC);
		return;
	}

	as = bch2_btree_update_start(iter->trans, iter->btree_id,
			btree_update_reserve_required(c, parent) + nr_old_nodes,
			BTREE_INSERT_NOFAIL|
			BTREE_INSERT_USE_RESERVE,
			NULL);
	if (IS_ERR(as)) {
		trace_btree_gc_coalesce_fail(c,
				BTREE_GC_COALESCE_FAIL_RESERVE_GET);
		bch2_keylist_free(&keylist, NULL);
		return;
	}

	trace_btree_gc_coalesce(c, old_nodes[0]);

	for (i = 0; i < nr_old_nodes; i++)
		bch2_btree_interior_update_will_free_node(as, old_nodes[i]);

	/* Repack everything with @new_format and sort down to one bset */
	for (i = 0; i < nr_old_nodes; i++)
		new_nodes[i] =
			__bch2_btree_node_alloc_replacement(as, old_nodes[i],
							    new_format);

	/*
	 * Conceptually we concatenate the nodes together and slice them
	 * up at different boundaries.
	 */
	for (i = nr_new_nodes - 1; i > 0; --i) {
		struct btree *n1 = new_nodes[i];
		struct btree *n2 = new_nodes[i - 1];

		struct bset *s1 = btree_bset_first(n1);
		struct bset *s2 = btree_bset_first(n2);
		struct bkey_packed *k, *last = NULL;

		/* Calculate how many keys from @n2 we could fit inside @n1 */
		u64s = 0;

		for (k = s2->start;
		     k < vstruct_last(s2) &&
		     vstruct_blocks_plus(n1->data, c->block_bits,
					 u64s + k->u64s) <= blocks;
		     k = bkey_next_skip_noops(k, vstruct_last(s2))) {
			last = k;
			u64s += k->u64s;
		}

		if (u64s == le16_to_cpu(s2->u64s)) {
			/* n2 fits entirely in n1 */
			n1->key.k.p = n1->data->max_key = n2->data->max_key;

			memcpy_u64s(vstruct_last(s1),
				    s2->start,
				    le16_to_cpu(s2->u64s));
			le16_add_cpu(&s1->u64s, le16_to_cpu(s2->u64s));

			set_btree_bset_end(n1, n1->set);

			six_unlock_write(&n2->c.lock);
			bch2_btree_node_free_never_inserted(c, n2);
			six_unlock_intent(&n2->c.lock);

			memmove(new_nodes + i - 1,
				new_nodes + i,
				sizeof(new_nodes[0]) * (nr_new_nodes - i));
			new_nodes[--nr_new_nodes] = NULL;
		} else if (u64s) {
			/* move part of n2 into n1 */
			n1->key.k.p = n1->data->max_key =
				bkey_unpack_pos(n1, last);

			n2->data->min_key = bkey_successor(n1->data->max_key);

			memcpy_u64s(vstruct_last(s1),
				    s2->start, u64s);
			le16_add_cpu(&s1->u64s, u64s);

			memmove(s2->start,
				vstruct_idx(s2, u64s),
				(le16_to_cpu(s2->u64s) - u64s) * sizeof(u64));
			s2->u64s = cpu_to_le16(le16_to_cpu(s2->u64s) - u64s);

			set_btree_bset_end(n1, n1->set);
			set_btree_bset_end(n2, n2->set);
		}
	}

	for (i = 0; i < nr_new_nodes; i++) {
		struct btree *n = new_nodes[i];

		recalc_packed_keys(n);
		btree_node_reset_sib_u64s(n);

		bch2_btree_build_aux_trees(n);

		bch2_btree_update_add_new_node(as, n);
		six_unlock_write(&n->c.lock);

		bch2_btree_node_write(c, n, SIX_LOCK_intent);
	}

	/*
	 * The keys for the old nodes get deleted. We don't want to insert keys
	 * that compare equal to the keys for the new nodes we'll also be
	 * inserting - we can't because keys on a keylist must be strictly
	 * greater than the previous keys, and we also don't need to since the
	 * key for the new node will serve the same purpose (overwriting the key
	 * for the old node).
	 */
	for (i = 0; i < nr_old_nodes; i++) {
		struct bkey_i delete;
		unsigned j;

		for (j = 0; j < nr_new_nodes; j++)
			if (!bkey_cmp(old_nodes[i]->key.k.p,
				      new_nodes[j]->key.k.p))
				goto next;

		bkey_init(&delete.k);
		delete.k.p = old_nodes[i]->key.k.p;
		bch2_keylist_add_in_order(&keylist, &delete);
next:
		continue;
	}

	/*
	 * Keys for the new nodes get inserted: bch2_btree_insert_keys() only
	 * does the lookup once and thus expects the keys to be in sorted order
	 * so we have to make sure the new keys are correctly ordered with
	 * respect to the deleted keys added in the previous loop
	 */
	for (i = 0; i < nr_new_nodes; i++)
		bch2_keylist_add_in_order(&keylist, &new_nodes[i]->key);

	/* Insert the newly coalesced nodes */
	bch2_btree_insert_node(as, parent, iter, &keylist, 0);

	BUG_ON(!bch2_keylist_empty(&keylist));

	BUG_ON(iter->l[old_nodes[0]->c.level].b != old_nodes[0]);

	bch2_btree_iter_node_replace(iter, new_nodes[0]);

	for (i = 0; i < nr_new_nodes; i++)
		bch2_btree_update_get_open_buckets(as, new_nodes[i]);

	/* Free the old nodes and update our sliding window */
	for (i = 0; i < nr_old_nodes; i++) {
		bch2_btree_node_free_inmem(c, old_nodes[i], iter);

		/*
		 * the index update might have triggered a split, in which case
		 * the nodes we coalesced - the new nodes we just created -
		 * might not be sibling nodes anymore - don't add them to the
		 * sliding window (except the first):
		 */
		if (!i) {
			old_nodes[i] = new_nodes[i];
		} else {
			old_nodes[i] = NULL;
		}
	}

	for (i = 0; i < nr_new_nodes; i++)
		six_unlock_intent(&new_nodes[i]->c.lock);

	bch2_btree_update_done(as);
	bch2_keylist_free(&keylist, NULL);
}

static int bch2_coalesce_btree(struct bch_fs *c, enum btree_id btree_id)
{
	struct btree_trans trans;
	struct btree_iter *iter;
	struct btree *b;
	bool kthread = (current->flags & PF_KTHREAD) != 0;
	unsigned i;

	/* Sliding window of adjacent btree nodes */
	struct btree *merge[GC_MERGE_NODES];
	u32 lock_seq[GC_MERGE_NODES];

	bch2_trans_init(&trans, c, 0, 0);

	/*
	 * XXX: We don't have a good way of positively matching on sibling nodes
	 * that have the same parent - this code works by handling the cases
	 * where they might not have the same parent, and is thus fragile. Ugh.
	 *
	 * Perhaps redo this to use multiple linked iterators?
	 */
	memset(merge, 0, sizeof(merge));

	__for_each_btree_node(&trans, iter, btree_id, POS_MIN,
			      BTREE_MAX_DEPTH, 0,
			      BTREE_ITER_PREFETCH, b) {
		memmove(merge + 1, merge,
			sizeof(merge) - sizeof(merge[0]));
		memmove(lock_seq + 1, lock_seq,
			sizeof(lock_seq) - sizeof(lock_seq[0]));

		merge[0] = b;

		for (i = 1; i < GC_MERGE_NODES; i++) {
			if (!merge[i] ||
			    !six_relock_intent(&merge[i]->c.lock, lock_seq[i]))
				break;

			if (merge[i]->c.level != merge[0]->c.level) {
				six_unlock_intent(&merge[i]->c.lock);
				break;
			}
		}
		memset(merge + i, 0, (GC_MERGE_NODES - i) * sizeof(merge[0]));

		bch2_coalesce_nodes(c, iter, merge);

		for (i = 1; i < GC_MERGE_NODES && merge[i]; i++) {
			lock_seq[i] = merge[i]->c.lock.state.seq;
			six_unlock_intent(&merge[i]->c.lock);
		}

		lock_seq[0] = merge[0]->c.lock.state.seq;

		if (kthread && kthread_should_stop()) {
			bch2_trans_exit(&trans);
			return -ESHUTDOWN;
		}

		bch2_trans_cond_resched(&trans);

		/*
		 * If the parent node wasn't relocked, it might have been split
		 * and the nodes in our sliding window might not have the same
		 * parent anymore - blow away the sliding window:
		 */
		if (btree_iter_node(iter, iter->level + 1) &&
		    !btree_node_intent_locked(iter, iter->level + 1))
			memset(merge + 1, 0,
			       (GC_MERGE_NODES - 1) * sizeof(merge[0]));
	}
	return bch2_trans_exit(&trans);
}

/**
 * bch_coalesce - coalesce adjacent nodes with low occupancy
 */
void bch2_coalesce(struct bch_fs *c)
{
	enum btree_id id;

	down_read(&c->gc_lock);
	trace_gc_coalesce_start(c);

	for (id = 0; id < BTREE_ID_NR; id++) {
		int ret = c->btree_roots[id].b
			? bch2_coalesce_btree(c, id)
			: 0;

		if (ret) {
			if (ret != -ESHUTDOWN)
				bch_err(c, "btree coalescing failed: %d", ret);
			return;
		}
	}

	trace_gc_coalesce_end(c);
	up_read(&c->gc_lock);
}

static int bch2_gc_thread(void *arg)
{
	struct bch_fs *c = arg;
	struct io_clock *clock = &c->io_clock[WRITE];
	unsigned long last = atomic64_read(&clock->now);
	unsigned last_kick = atomic_read(&c->kick_gc);
	int ret;

	set_freezable();

	while (1) {
		while (1) {
			set_current_state(TASK_INTERRUPTIBLE);

			if (kthread_should_stop()) {
				__set_current_state(TASK_RUNNING);
				return 0;
			}

			if (atomic_read(&c->kick_gc) != last_kick)
				break;

			if (c->btree_gc_periodic) {
				unsigned long next = last + c->capacity / 16;

				if (atomic64_read(&clock->now) >= next)
					break;

				bch2_io_clock_schedule_timeout(clock, next);
			} else {
				schedule();
			}

			try_to_freeze();
		}
		__set_current_state(TASK_RUNNING);

		last = atomic64_read(&clock->now);
		last_kick = atomic_read(&c->kick_gc);

		/*
		 * Full gc is currently incompatible with btree key cache:
		 */
#if 0
		ret = bch2_gc(c, false, false);
#else
		ret = bch2_gc_gens(c);
#endif
		if (ret < 0)
			bch_err(c, "btree gc failed: %i", ret);

		debug_check_no_locks_held();
	}

	return 0;
}

void bch2_gc_thread_stop(struct bch_fs *c)
{
	struct task_struct *p;

	p = c->gc_thread;
	c->gc_thread = NULL;

	if (p) {
		kthread_stop(p);
		put_task_struct(p);
	}
}

int bch2_gc_thread_start(struct bch_fs *c)
{
	struct task_struct *p;

	BUG_ON(c->gc_thread);

	p = kthread_create(bch2_gc_thread, c, "bch-gc/%s", c->name);
	if (IS_ERR(p)) {
		bch_err(c, "error creating gc thread: %li", PTR_ERR(p));
		return PTR_ERR(p);
	}

	get_task_struct(p);
	c->gc_thread = p;
	wake_up_process(p);
	return 0;
}
