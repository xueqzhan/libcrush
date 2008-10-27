
#include <linux/radix-tree.h>
#include <linux/sort.h>

#include "ceph_debug.h"

int ceph_debug_snap = -1;
#define DOUT_MASK DOUT_MASK_SNAP
#define DOUT_VAR ceph_debug_snap
#define DOUT_PREFIX "snap: "

#include "super.h"
#include "decode.h"

/*
 * Snapshots in ceph are driven in large part by cooperation from the
 * client.  In contrast to local file systems or file servers that
 * implement snapshots at a single point in the system, ceph's
 * distributed access to storage requires clients to help decide
 * whether a write logically occurs before or after a recently created
 * snapshot.
 *
 * This provides a perfect instantanous client-wide snapshot.  Between
 * clients, however, snapshots may appear to be applied at slightly
 * different points in time, depending on delays in delivering the
 * snapshot notification.
 *
 * Snapshots are _not_ file system-wide.  Instead, each snapshot
 * applies to the subdirectory nested beneath some directory.  This
 * effectively divides the hierarchy into multiple "realms," where all
 * of the files contained by each realm share the same set of
 * snapshots.  An individual realm's snap set contains snapshots
 * explicitly created on that realm, as well as any snaps in its
 * parent's snap set _after_ the point at which the parent became it's
 * parent (due to, say, a rename).  Similarly, snaps from prior parents
 * during the time intervals during which they were the parent are included.
 *
 * The client is spared most of this detail, fortunately... it must only
 * maintains a hierarchy of realms reflecting the current parent/child
 * realm relationship, and for each realm has an explicit list of snaps
 * inherited from prior parents.
 *
 * A snap_realm struct is maintained for realms containing every inode
 * with an open cap in the system.  (The needed snap realm information is
 * provided by the MDS whenever a cap is issued, i.e., on open.)  A 'seq'
 * version number is used to ensure that as realm parameters change (new
 * snapshot, new parent, etc.) the client's realm hierarchy is updated.
 *
 * The realm hierarchy drives the generation of a 'snap context' for each
 * realm, which simply lists the resulting set of snaps for the realm.  This
 * is attached to any writes sent to OSDs.
 */

/*
 * Unfortunately error handling is a bit mixed here.  If we get a snap
 * update, but don't have enough memory to update our realm hierarchy,
 * it's not clear what we can do about it (besides complaining to the
 * console).
 */

/*
 * find/create the realm rooted at @ino and bump its ref count.
 *
 * caller must hold snap_rwsem for write.
 */
static
struct ceph_snap_realm *ceph_get_snap_realm(struct ceph_mds_client *mdsc,
					    u64 ino)
{
	struct ceph_snap_realm *realm;

	realm = radix_tree_lookup(&mdsc->snap_realms, ino);
	if (!realm) {
		realm = kzalloc(sizeof(*realm), GFP_NOFS);
		if (!realm)
			return ERR_PTR(-ENOMEM);
		radix_tree_insert(&mdsc->snap_realms, ino, realm);
		realm->nref = 0;    /* tree does not take a ref */
		realm->ino = ino;
		INIT_LIST_HEAD(&realm->children);
		INIT_LIST_HEAD(&realm->child_item);
		INIT_LIST_HEAD(&realm->inodes_with_caps);
		dout(20, "get_snap_realm created %llx %p\n", realm->ino, realm);
	}
	dout(20, "get_snap_realm %llx %p %d -> %d\n", realm->ino, realm,
	     realm->nref, realm->nref+1);
	realm->nref++;
	return realm;
}

/*
 * caller must hold snap_rwsem for write
 */
void ceph_put_snap_realm(struct ceph_mds_client *mdsc,
			 struct ceph_snap_realm *realm)
{
	dout(20, "put_snap_realm %llx %p %d -> %d\n", realm->ino, realm,
	     realm->nref, realm->nref-1);
	realm->nref--;
	if (realm->nref == 0) {
		if (realm->parent) {
			list_del_init(&realm->child_item);
			ceph_put_snap_realm(mdsc, realm->parent);
		}
		radix_tree_delete(&mdsc->snap_realms, realm->ino);
		kfree(realm->prior_parent_snaps);
		kfree(realm->snaps);
		ceph_put_snap_context(realm->cached_context);
		kfree(realm);
	}
}

/*
 * adjust the parent realm of a given @realm.  adjust child list, and parent
 * pointers, and ref counts appropriately.
 *
 * return true if parent was changed, 0 if unchanged, <0 on error.
 *
 * caller must hold snap_rwsem for write.
 */
static int adjust_snap_realm_parent(struct ceph_mds_client *mdsc,
				    struct ceph_snap_realm *realm,
				    u64 parentino)
{
	struct ceph_snap_realm *parent;

	if (realm->parent_ino == parentino)
		return 0;

	parent = ceph_get_snap_realm(mdsc, parentino);
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	dout(20, "adjust_snap_realm_parent %llx %p: %llx %p -> %llx %p\n",
	     realm->ino, realm, realm->parent_ino, realm->parent,
	     parentino, parent);
	if (realm->parent) {
		list_del_init(&realm->child_item);
		ceph_put_snap_realm(mdsc, realm->parent);
	}
	realm->parent_ino = parentino;
	realm->parent = parent;
	list_add(&realm->child_item, &parent->children);
	return 1;
}


static int cmpu64_rev(const void *a, const void *b)
{
	if (*(u64 *)a < *(u64 *)b)
		return 1;
	if (*(u64 *)a > *(u64 *)b)
		return -1;
	return 0;
}

/*
 * build the snap context for a given realm.
 */
static int build_snap_context(struct ceph_snap_realm *realm)
{
	struct ceph_snap_realm *parent = realm->parent;
	struct ceph_snap_context *snapc;
	int err = 0;
	int i;
	int num = realm->num_prior_parent_snaps + realm->num_snaps;

	/*
	 * build parent context, if it hasn't been built.
	 * conservatively estimate that all parent snaps might be
	 * included by us.
	 */
	if (parent) {
		if (!parent->cached_context) {
			err = build_snap_context(parent);
			if (err)
				goto fail;
		}
		num += parent->cached_context->num_snaps;
	}

	/* do i actually need to update?  not if my context seq
	   matches realm seq, and my parents' does to.  (this works
	   because we rebuild_snap_realms() works _downward_ in
	   hierarchy after each update.) */
	if (realm->cached_context &&
	    realm->cached_context->seq <= realm->seq &&
	    (!parent ||
	     realm->cached_context->seq <= parent->cached_context->seq)) {
		dout(10, "build_snap_context %llx %p: %p seq %lld (%d snaps)"
		     " (unchanged)\n",
		     realm->ino, realm, realm->cached_context,
		     realm->cached_context->seq,
		     realm->cached_context->num_snaps);
		return 0;
	}

	/* alloc new snap context */
	err = -ENOMEM;
	snapc = kzalloc(sizeof(*snapc) + num*sizeof(u64), GFP_NOFS);
	if (!snapc)
		goto fail;
	atomic_set(&snapc->nref, 1);

	/* build (reverse sorted) snap vector */
	num = 0;
	snapc->seq = realm->seq;
	if (parent) {
		/* include any of parent's snaps occuring _after_ my
		   parent became my parent */
		for (i = 0; i < parent->cached_context->num_snaps; i++)
			if (parent->cached_context->snaps[i] >=
			    realm->parent_since)
				snapc->snaps[num++] =
					parent->cached_context->snaps[i];
		if (parent->cached_context->seq > snapc->seq)
			snapc->seq = parent->cached_context->seq;
	}
	memcpy(snapc->snaps + num, realm->snaps,
	       sizeof(u64)*realm->num_snaps);
	num += realm->num_snaps;
	memcpy(snapc->snaps + num, realm->prior_parent_snaps,
	       sizeof(u64)*realm->num_prior_parent_snaps);
	num += realm->num_prior_parent_snaps;

	sort(snapc->snaps, num, sizeof(u64), cmpu64_rev, NULL);
	snapc->num_snaps = num;
	dout(10, "build_snap_context %llx %p: %p seq %lld (%d snaps)\n",
	     realm->ino, realm, snapc, snapc->seq, snapc->num_snaps);

	if (realm->cached_context)
		ceph_put_snap_context(realm->cached_context);
	realm->cached_context = snapc;
	return 0;

fail:
	/*
	 * if we fail, clear old (incorrect) cached_context... hopefully
	 * we'll have better luck building it later
	 */
	if (realm->cached_context) {
		ceph_put_snap_context(realm->cached_context);
		realm->cached_context = NULL;
	}
	derr(0, "build_snap_context %llx %p fail %d\n", realm->ino,
	     realm, err);
	return err;
}

/*
 * rebuild snap context for the given realm and all of its children.
 */
static void rebuild_snap_realms(struct ceph_snap_realm *realm)
{
	struct list_head *p;
	struct ceph_snap_realm *child;

	dout(10, "rebuild_snap_realms %llx %p\n", realm->ino, realm);
	build_snap_context(realm);

	list_for_each(p, &realm->children) {
		child = list_entry(p, struct ceph_snap_realm, child_item);
		rebuild_snap_realms(child);
	}
}


/*
 * helper to allocate and decode an array of snapids.  free prior
 * instance, if any.
 */
static int dup_array(u64 **dst, __le64 *src, int num)
{
	int i;

	if (*dst)
		kfree(*dst);
	if (num) {
		*dst = kmalloc(sizeof(u64) * num, GFP_NOFS);
		if (!*dst)
			return -ENOMEM;
		for (i = 0; i < num; i++)
			(*dst)[i] = le64_to_cpu(src[i]);
	} else {
		*dst = NULL;
	}
	return 0;
}


/*
 * When a snapshot is applied, the size/mtime inode metadata is queued
 * in a ceph_cap_snap (one for each snapshot) until writeback
 * completes and the metadata can be flushed back to the MDS.
 *
 * However, if a (sync) write is currently in-progress when we apply
 * the snapshot, we have to wait until the write succeeds or fails
 * (and a final size/mtime is known).  In this case the
 * cap_snap->writing = 1, and is said to be "pending."  When the write
 * finishes, we __ceph_finish_cap_snap().
 *
 * Caller must hold snap_rwsem for read (i.e., the realm topology won't
 * change).
 */
void ceph_queue_cap_snap(struct ceph_inode_info *ci,
			 struct ceph_snap_context *snapc)
{
	struct inode *inode = &ci->vfs_inode;
	struct ceph_cap_snap *capsnap;
	int used;

	capsnap = kzalloc(sizeof(*capsnap), GFP_NOFS);
	if (!capsnap) {
		derr(10, "ENOMEM allocating ceph_cap_snap on %p\n", inode);
		return;
	}

	spin_lock(&inode->i_lock);
	used = __ceph_caps_used(ci);
	if (__ceph_have_pending_cap_snap(ci)) {
		/* there is no point in queuing multiple "pending" cap_snaps,
		   as no new writes are allowed to start when pending, so any
		   writes in progress now were started before the previous
		   cap_snap.  lucky us. */
		dout(10, "queue_cap_snap %p snapc %p seq %llu used %d"
		     " already pending\n", inode, snapc, snapc->seq, used);
		kfree(capsnap);
	} else {
		igrab(inode);
		capsnap->follows = snapc->seq - 1;
		capsnap->context = ceph_get_snap_context(snapc);
		capsnap->issued = __ceph_caps_issued(ci, NULL);
		/* dirty page count moved from _head to this cap_snap;
		   all subsequent writes page dirties occur _after_ this
		   snapshot. */
		capsnap->dirty = ci->i_wrbuffer_ref_head;
		ci->i_wrbuffer_ref_head = 0;
		list_add_tail(&capsnap->ci_item, &ci->i_cap_snaps);

		if (used & CEPH_CAP_WR) {
			dout(10, "queue_cap_snap %p cap_snap %p snapc %p"
			     " seq %llu used WR, now pending\n", inode,
			     capsnap, snapc, snapc->seq);
			capsnap->writing = 1;
		} else {
			/* note mtime, size NOW. */
			__ceph_finish_cap_snap(ci, capsnap);
		}
	}

	spin_unlock(&inode->i_lock);
}

/*
 * Finalize the size, mtime for a cap_snap.. that is, settle on final values
 * to be used for the snapshot, to be flushed back to the mds.
 *
 * If capsnap can now be flushed, add to snap_flush list, and return 1.
 *
 * Caller must hold i_lock.
 */
int __ceph_finish_cap_snap(struct ceph_inode_info *ci,
			    struct ceph_cap_snap *capsnap)
{
	struct inode *inode = &ci->vfs_inode;
	struct ceph_mds_client *mdsc = &ceph_client(inode->i_sb)->mdsc;

	BUG_ON(capsnap->writing);
	capsnap->size = inode->i_size;
	capsnap->mtime = inode->i_mtime;
	capsnap->atime = inode->i_atime;
	capsnap->ctime = inode->i_ctime;
	capsnap->time_warp_seq = ci->i_time_warp_seq;
	if (capsnap->dirty) {
		dout(10, "finish_cap_snap %p cap_snap %p snapc %p %llu s=%llu "
		     "still has %d dirty pages\n", inode, capsnap,
		     capsnap->context, capsnap->context->seq,
		     capsnap->size, capsnap->dirty);
		return 0;
	}
	dout(10, "finish_cap_snap %p cap_snap %p snapc %p %llu s=%llu clean\n",
	     inode, capsnap, capsnap->context,
	     capsnap->context->seq, capsnap->size);

	spin_lock(&mdsc->snap_flush_lock);
	list_add_tail(&ci->i_snap_flush_item, &mdsc->snap_flush_list);
	spin_unlock(&mdsc->snap_flush_lock);
	return 1;  /* caller may want to ceph_flush_snaps */
}


/*
 * Parse and apply a snapblob "snap trace" from the MDS.  This specifies
 * the snap realm parameters from a given realm and all of its ancestors,
 * up to the root.
 *
 * Caller must hold snap_rwsem for write.
 */
struct ceph_snap_realm *ceph_update_snap_trace(struct ceph_mds_client *mdsc,
					       void *p, void *e, bool deletion)
{
	struct ceph_mds_snap_realm *ri;    /* encoded */
	__le64 *snaps;                     /* encoded */
	__le64 *prior_parent_snaps;        /* encoded */
	struct ceph_snap_realm *realm, *first = NULL;
	int invalidate = 0;
	int err = -ENOMEM;

	dout(10, "update_snap_trace deletion=%d\n", deletion);
more:
	ceph_decode_need(&p, e, sizeof(*ri), bad);
	ri = p;
	p += sizeof(*ri);
	ceph_decode_need(&p, e, sizeof(u64)*(le32_to_cpu(ri->num_snaps) +
			    le32_to_cpu(ri->num_prior_parent_snaps)), bad);
	snaps = p;
	p += sizeof(u64) * le32_to_cpu(ri->num_snaps);
	prior_parent_snaps = p;
	p += sizeof(u64) * le32_to_cpu(ri->num_prior_parent_snaps);

	realm = ceph_get_snap_realm(mdsc, le64_to_cpu(ri->ino));
	if (IS_ERR(realm)) {
		err = PTR_ERR(realm);
		goto fail;
	}
	if (!first) {
		/* take note if this is the first realm in the trace
		 * (the most deeply nested)... we will return if (with
		 * nref bumped) to the caller. */
		first = realm;
		realm->nref++;
	}

	if (le64_to_cpu(ri->seq) > realm->seq) {
		dout(10, "update_snap_trace updating %llx %p %lld -> %lld\n",
		     realm->ino, realm, realm->seq, le64_to_cpu(ri->seq));
		/*
		 * if the realm seq has changed, queue a cap_snap for every
		 * inode with open caps.  we do this _before_ we update
		 * the realm info so that we prepare for writeback under the
		 * _previous_ snap context.
		 *
		 * ...unless it's a snap deletion!
		 */
		if (!deletion) {
			struct list_head *pi;

			list_for_each(pi, &realm->inodes_with_caps) {
				struct ceph_inode_info *ci =
					list_entry(pi, struct ceph_inode_info,
						   i_snap_realm_item);
				ceph_queue_cap_snap(ci, realm->cached_context);
			}
			dout(20, "update_snap_trace cap_snaps queued\n");
		}

	} else {
		dout(10, "update_snap_trace %llx %p seq %lld unchanged\n",
		     realm->ino, realm, realm->seq);
	}

	/* ensure the parent is correct */
	err = adjust_snap_realm_parent(mdsc, realm, le64_to_cpu(ri->parent));
	if (err < 0)
		goto fail;
	invalidate += err;

	if (le64_to_cpu(ri->seq) > realm->seq) {
		/* update realm parameters, snap lists */
		realm->seq = le64_to_cpu(ri->seq);
		realm->created = le64_to_cpu(ri->created);
		realm->parent_since = le64_to_cpu(ri->parent_since);

		realm->num_snaps = le32_to_cpu(ri->num_snaps);
		err = dup_array(&realm->snaps, snaps, realm->num_snaps);
		if (err < 0)
			goto fail;

		realm->num_prior_parent_snaps =
			le32_to_cpu(ri->num_prior_parent_snaps);
		err = dup_array(&realm->prior_parent_snaps, prior_parent_snaps,
				realm->num_prior_parent_snaps);
		if (err < 0)
			goto fail;

		invalidate = 1;
	} else if (!realm->cached_context) {
		invalidate = 1;
	}

	dout(10, "done with %llx %p, invalidated=%d, %p %p\n", realm->ino,
	     realm, invalidate, p, e);

	/* invalidate when we reach the _end_ (root) of the trace */
	if (p == e && invalidate)
		rebuild_snap_realms(realm);

	ceph_put_snap_realm(mdsc, realm);
	if (p < e)
		goto more;

	return first;

bad:
	err = -EINVAL;
fail:
	derr(10, "update_snap_trace error %d\n", err);
	return ERR_PTR(err);
}


/*
 * Send any cap_snaps that are queued for flush.  Try to carry
 * s_mutex across multiple snap flushes to avoid locking overhead.
 *
 * Caller holds no locks.
 */
static void flush_snaps(struct ceph_mds_client *mdsc)
{
	struct ceph_inode_info *ci;
	struct inode *inode;
	struct ceph_mds_session *session = NULL;

	dout(10, "flush_snaps\n");
	spin_lock(&mdsc->snap_flush_lock);
	while (!list_empty(&mdsc->snap_flush_list)) {
		ci = list_entry(mdsc->snap_flush_list.next,
				struct ceph_inode_info, i_snap_flush_item);
		inode = &ci->vfs_inode;
		igrab(inode);
		spin_unlock(&mdsc->snap_flush_lock);
		spin_lock(&inode->i_lock);
		__ceph_flush_snaps(ci, &session);
		spin_unlock(&inode->i_lock);
		iput(inode);
		spin_lock(&mdsc->snap_flush_lock);
	}
	spin_unlock(&mdsc->snap_flush_lock);

	if (session) {
		mutex_unlock(&session->s_mutex);
		ceph_put_mds_session(session);
	}
	dout(10, "flush_snaps done\n");
}


/*
 * Handle a snap notification from the MDS.
 *
 * This can take two basic forms: the simplest is just a snap creation
 * or deletion notification on an existing realm.  This should update the
 * realm and its children.
 *
 * The more difficult case is realm creation, due to snap creation at a
 * new point in the file hierarchy, or due to a rename that moves a file or
 * directory into another realm.
 */
void ceph_handle_snap(struct ceph_mds_client *mdsc,
		      struct ceph_msg *msg)
{
	struct super_block *sb = mdsc->client->sb;
	struct ceph_mds_session *session;
	int mds;
	u64 split;
	int op;
	int trace_len;
	struct ceph_snap_realm *realm = NULL;
	void *p = msg->front.iov_base;
	void *e = p + msg->front.iov_len;
	struct ceph_mds_snap_head *h;
	int num_split_inos, num_split_realms;
	__le64 *split_inos = NULL, *split_realms = NULL;
	int i;
	int locked_rwsem = 0;

	if (le32_to_cpu(msg->hdr.src.name.type) != CEPH_ENTITY_TYPE_MDS)
		return;
	mds = le32_to_cpu(msg->hdr.src.name.num);

	/* decode */
	if (msg->front.iov_len < sizeof(*h))
		goto bad;
	h = p;
	op = le32_to_cpu(h->op);
	split = le64_to_cpu(h->split);   /* non-zero if we are splitting an
					  * existing realm */
	num_split_inos = le32_to_cpu(h->num_split_inos);
	num_split_realms = le32_to_cpu(h->num_split_realms);
	trace_len = le32_to_cpu(h->trace_len);
	p += sizeof(*h);

	dout(10, "handle_snap from mds%d op %s split %llx tracelen %d\n", mds,
	     ceph_snap_op_name(op), split, trace_len);

	/* find session */
	mutex_lock(&mdsc->mutex);
	session = __ceph_get_mds_session(mdsc, mds);
	if (session)
		down_write(&mdsc->snap_rwsem);
	mutex_unlock(&mdsc->mutex);
	if (!session) {
		dout(10, "WTF, got snap but no session for mds%d\n", mds);
		return;
	}
	locked_rwsem = 1;

	mutex_lock(&session->s_mutex);
	session->s_seq++;
	mutex_unlock(&session->s_mutex);

	if (op == CEPH_SNAP_OP_SPLIT) {
		struct ceph_mds_snap_realm *ri;

		/*
		 * A "split" breaks part of an existing realm off into
		 * a new realm.  The MDS provides a list of inodes
		 * (with caps) and child realms that belong to the new
		 * child.
		 */
		split_inos = p;
		p += sizeof(u64) * num_split_inos;
		split_realms = p;
		p += sizeof(u64) * num_split_realms;
		ceph_decode_need(&p, e, sizeof(*ri), bad);
		/* we will peek at realm info here, but will _not_
		 * advance p, as the realm update will occur below in
		 * ceph_update_snap_trace. */
		ri = p;

		realm = ceph_get_snap_realm(mdsc, split);
		if (IS_ERR(realm))
			goto out;

		dout(10, "splitting snap_realm %llx %p\n", realm->ino, realm);
		for (i = 0; i < num_split_inos; i++) {
			struct ceph_vino vino = {
				.ino = le64_to_cpu(split_inos[i]),
				.snap = CEPH_NOSNAP,
			};
			struct inode *inode = ceph_find_inode(sb, vino);
			struct ceph_inode_info *ci;

			if (!inode)
				continue;
			ci = ceph_inode(inode);

			spin_lock(&inode->i_lock);
			if (!ci->i_snap_realm)
				goto skip_inode;
			/*
			 * If this inode belongs to a realm that was
			 * created after our new realm, we experienced
			 * a race (due to another split notifications
			 * arriving from a different MDS).  So skip
			 * this inode.
			 */
			if (ci->i_snap_realm->created >
			    le64_to_cpu(ri->created)) {
				dout(15, " leaving %p in newer realm %llx %p\n",
				     inode, ci->i_snap_realm->ino,
				     ci->i_snap_realm);
				goto skip_inode;
			}
			dout(15, " will move %p to split realm %llx %p\n",
			     inode, realm->ino, realm);
			/*
			 * Remove the inode from the realm's inode
			 * list, but don't add it to the new realm
			 * yet.  We don't want the cap_snap to be
			 * queued (again) by ceph_update_snap_trace()
			 * below.  Queue it _now_, under the old context.
			 */
			list_del_init(&ci->i_snap_realm_item);
			spin_unlock(&inode->i_lock);

			ceph_queue_cap_snap(ci,
					    ci->i_snap_realm->cached_context);

			iput(inode);
			continue;

		skip_inode:
			spin_unlock(&inode->i_lock);
			iput(inode);
		}

		/* we may have taken some of the old realm's children. */
		for (i = 0; i < num_split_realms; i++) {
			struct ceph_snap_realm *child =
				ceph_get_snap_realm(mdsc,
					   le64_to_cpu(split_realms[i]));
			if (IS_ERR(child))
				continue;
			adjust_snap_realm_parent(mdsc, child, realm->ino);
			ceph_put_snap_realm(mdsc, child);
		}
	}

	/*
	 * update using the provided snap trace. if we are deleting a
	 * snap, we can avoid queueing cap_snaps.
	 */
	realm = ceph_update_snap_trace(mdsc, p, e,
				       op == CEPH_SNAP_OP_DESTROY);
	if (IS_ERR(realm))
		goto bad;

	if (op == CEPH_SNAP_OP_SPLIT) {
		/*
		 * ok, _now_ add the inodes into the new realm.
		 */
		for (i = 0; i < num_split_inos; i++) {
			struct ceph_vino vino = {
				.ino = le64_to_cpu(split_inos[i]),
				.snap = CEPH_NOSNAP,
			};
			struct inode *inode = ceph_find_inode(sb, vino);
			struct ceph_inode_info *ci;

			if (!inode)
				continue;
			ci = ceph_inode(inode);
			spin_lock(&inode->i_lock);
			ceph_put_snap_realm(mdsc, ci->i_snap_realm);
			list_add(&ci->i_snap_realm_item,
				 &realm->inodes_with_caps);
			ci->i_snap_realm = realm;
			realm->nref++;
			spin_unlock(&inode->i_lock);
			iput(inode);
		}

		/* we took a reference when we created the realm, above */
		ceph_put_snap_realm(mdsc, realm);
	}

	ceph_put_snap_realm(mdsc, realm);
	up_write(&mdsc->snap_rwsem);

	flush_snaps(mdsc);
	return;

bad:
	derr(10, "corrupt snap message from mds%d\n", mds);
out:
	if (locked_rwsem)
		up_write(&mdsc->snap_rwsem);
	return;
}



