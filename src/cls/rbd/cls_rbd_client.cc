// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "cls/rbd/cls_rbd_client.h"
#include "cls/lock/cls_lock_client.h"
#include "include/buffer.h"
#include "include/Context.h"
#include "include/encoding.h"
#include "include/rbd_types.h"
#include "common/Cond.h"

#include <errno.h>

namespace librbd {
  namespace cls_client {

    void get_immutable_metadata_start(librados::ObjectReadOperation *op) {
      bufferlist bl, empty_bl;
      snapid_t snap = CEPH_NOSNAP;
      ::encode(snap, bl);
      op->exec("rbd", "get_size", bl);
      op->exec("rbd", "get_object_prefix", empty_bl);
    }

    int get_immutable_metadata_finish(bufferlist::iterator *it,
                                      std::string *object_prefix,
                                      uint8_t *order) {
      try {
	uint64_t size;
	// get_size
	::decode(*order, *it);
	::decode(size, *it);
	// get_object_prefix
	::decode(*object_prefix, *it);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }
      return 0;

    }

    int get_immutable_metadata(librados::IoCtx *ioctx, const std::string &oid,
			       std::string *object_prefix, uint8_t *order)
    {
      librados::ObjectReadOperation op;
      get_immutable_metadata_start(&op);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
        return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return get_immutable_metadata_finish(&it, object_prefix, order);
    }

    void get_mutable_metadata_start(librados::ObjectReadOperation *op,
                                    bool read_only) {
      snapid_t snap = CEPH_NOSNAP;
      bufferlist size_bl;
      ::encode(snap, size_bl);
      op->exec("rbd", "get_size", size_bl);

      bufferlist features_bl;
      ::encode(snap, features_bl);
      ::encode(read_only, features_bl);
      op->exec("rbd", "get_features", features_bl);

      bufferlist empty_bl;
      op->exec("rbd", "get_snapcontext", empty_bl);

      bufferlist parent_bl;
      ::encode(snap, parent_bl);
      op->exec("rbd", "get_parent", parent_bl);
      rados::cls::lock::get_lock_info_start(op, RBD_LOCK_NAME);
    }

    int get_mutable_metadata_finish(bufferlist::iterator *it,
                                    uint64_t *size, uint64_t *features,
                                    uint64_t *incompatible_features,
                                    std::map<rados::cls::lock::locker_id_t,
                                             rados::cls::lock::locker_info_t> *lockers,
                                    bool *exclusive_lock, std::string *lock_tag,
                                    ::SnapContext *snapc, parent_info *parent) {
      assert(size);
      assert(features);
      assert(incompatible_features);
      assert(lockers);
      assert(exclusive_lock);
      assert(snapc);
      assert(parent);

      try {
	uint8_t order;
	// get_size
	::decode(order, *it);
	::decode(*size, *it);
	// get_features
	::decode(*features, *it);
	::decode(*incompatible_features, *it);
	// get_snapcontext
	::decode(*snapc, *it);
	// get_parent
	::decode(parent->spec.pool_id, *it);
	::decode(parent->spec.image_id, *it);
	::decode(parent->spec.snap_id, *it);
	::decode(parent->overlap, *it);

	// get_lock_info
	ClsLockType lock_type = LOCK_NONE;
	int r = rados::cls::lock::get_lock_info_finish(it, lockers, &lock_type,
						       lock_tag);
        if (r == -EOPNOTSUPP) {
          r = 0;
        }
        if (r == 0) {
	  *exclusive_lock = (lock_type == LOCK_EXCLUSIVE);
        }
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }
      return 0;
    }

    int get_mutable_metadata(librados::IoCtx *ioctx, const std::string &oid,
			     bool read_only, uint64_t *size, uint64_t *features,
			     uint64_t *incompatible_features,
			     map<rados::cls::lock::locker_id_t,
				 rados::cls::lock::locker_info_t> *lockers,
                             bool *exclusive_lock,
			     string *lock_tag,
			     ::SnapContext *snapc,
			     parent_info *parent)
    {
      librados::ObjectReadOperation op;
      get_mutable_metadata_start(&op, read_only);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
        return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return get_mutable_metadata_finish(&it, size, features,
                                         incompatible_features, lockers,
                                         exclusive_lock, lock_tag, snapc,
                                         parent);
    }

    int create_image(librados::IoCtx *ioctx, const std::string &oid,
		     uint64_t size, uint8_t order, uint64_t features,
		     const std::string &object_prefix)
    {
      bufferlist bl, bl2;
      ::encode(size, bl);
      ::encode(order, bl);
      ::encode(features, bl);
      ::encode(object_prefix, (bl));

      return ioctx->exec(oid, "rbd", "create", bl, bl2);
    }

    int get_features(librados::IoCtx *ioctx, const std::string &oid,
		     snapid_t snap_id, uint64_t *features)
    {
      bufferlist inbl, outbl;
      ::encode(snap_id, inbl);

      int r = ioctx->exec(oid, "rbd", "get_features", inbl, outbl);
      if (r < 0)
	return r;

      try {
	bufferlist::iterator iter = outbl.begin();
	::decode(*features, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      return 0;
    }

    int set_features(librados::IoCtx *ioctx, const std::string &oid,
                      uint64_t features, uint64_t mask)
    {
      bufferlist inbl;
      ::encode(features, inbl);
      ::encode(mask, inbl);

      librados::ObjectWriteOperation op;
      op.exec("rbd", "set_features", inbl);
      return ioctx->operate(oid, &op);
    }

    int get_object_prefix(librados::IoCtx *ioctx, const std::string &oid,
			  std::string *object_prefix)
    {
      bufferlist inbl, outbl;
      int r = ioctx->exec(oid, "rbd", "get_object_prefix", inbl, outbl);
      if (r < 0)
	return r;

      try {
	bufferlist::iterator iter = outbl.begin();
	::decode(*object_prefix, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      return 0;
    }

    int get_size(librados::IoCtx *ioctx, const std::string &oid,
		 snapid_t snap_id, uint64_t *size, uint8_t *order)
    {
      bufferlist inbl, outbl;
      ::encode(snap_id, inbl);

      int r = ioctx->exec(oid, "rbd", "get_size", inbl, outbl);
      if (r < 0)
	return r;

      try {
	bufferlist::iterator iter = outbl.begin();
	::decode(*order, iter);
	::decode(*size, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      return 0;
    }

    int set_size(librados::IoCtx *ioctx, const std::string &oid,
                 uint64_t size)
    {
      librados::ObjectWriteOperation op;
      set_size(&op, size);
      return ioctx->operate(oid, &op);
    }

    void set_size(librados::ObjectWriteOperation *op, uint64_t size)
    {
      bufferlist bl;
      ::encode(size, bl);
      op->exec("rbd", "set_size", bl);
    }

    int get_parent(librados::IoCtx *ioctx, const std::string &oid,
		   snapid_t snap_id, parent_spec *pspec, 
		   uint64_t *parent_overlap)
    {
      bufferlist inbl, outbl;
      ::encode(snap_id, inbl);

      int r = ioctx->exec(oid, "rbd", "get_parent", inbl, outbl);
      if (r < 0)
	return r;

      try {
	bufferlist::iterator iter = outbl.begin();
	::decode(pspec->pool_id, iter);
	::decode(pspec->image_id, iter);
	::decode(pspec->snap_id, iter);
	::decode(*parent_overlap, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      return 0;
    }

    int set_parent(librados::IoCtx *ioctx, const std::string &oid,
		   parent_spec pspec, uint64_t parent_overlap)
    {
      bufferlist inbl, outbl;
      ::encode(pspec.pool_id, inbl);
      ::encode(pspec.image_id, inbl);
      ::encode(pspec.snap_id, inbl);
      ::encode(parent_overlap, inbl);

      return ioctx->exec(oid, "rbd", "set_parent", inbl, outbl);
    }

    void get_flags_start(librados::ObjectReadOperation *op,
                         const std::vector<snapid_t> &snap_ids) {
      bufferlist in_bl;
      ::encode(static_cast<snapid_t>(CEPH_NOSNAP), in_bl);

      op->exec("rbd", "get_flags", in_bl);
      for (size_t i = 0; i < snap_ids.size(); ++i) {
        bufferlist snap_bl;
        ::encode(snap_ids[i], snap_bl);
        op->exec("rbd", "get_flags", snap_bl);
      }

    }

    int get_flags_finish(bufferlist::iterator *it, uint64_t *flags,
                         const std::vector<snapid_t> &snap_ids,
                         std::vector<uint64_t> *snap_flags) {
      snap_flags->resize(snap_ids.size());
      try {
        ::decode(*flags, *it);
	for (size_t i = 0; i < snap_flags->size(); ++i) {
	  ::decode((*snap_flags)[i], *it);
	}
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }
      return 0;
    }

    int get_flags(librados::IoCtx *ioctx, const std::string &oid,
		  uint64_t *flags, const std::vector<snapid_t> &snap_ids,
		  vector<uint64_t> *snap_flags)
    {
      librados::ObjectReadOperation op;
      get_flags_start(&op, snap_ids);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
        return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return get_flags_finish(&it, flags, snap_ids, snap_flags);
    }

    void set_flags(librados::ObjectWriteOperation *op, snapid_t snap_id,
                   uint64_t flags, uint64_t mask)
    {
      bufferlist inbl;
      ::encode(flags, inbl);
      ::encode(mask, inbl);
      ::encode(snap_id, inbl);
      op->exec("rbd", "set_flags", inbl);
    }

    int remove_parent(librados::IoCtx *ioctx, const std::string &oid)
    {
      librados::ObjectWriteOperation op;
      remove_parent(&op);
      return ioctx->operate(oid, &op);
    }

    void remove_parent(librados::ObjectWriteOperation *op)
    {
      bufferlist inbl;
      op->exec("rbd", "remove_parent", inbl);
    }

    int add_child(librados::IoCtx *ioctx, const std::string &oid,
		  parent_spec pspec, const std::string &c_imageid)
    {
      bufferlist in, out;
      ::encode(pspec.pool_id, in);
      ::encode(pspec.image_id, in);
      ::encode(pspec.snap_id, in);
      ::encode(c_imageid, in);

      return ioctx->exec(oid, "rbd", "add_child", in, out);
    }

    void remove_child(librados::ObjectWriteOperation *op,
		      parent_spec pspec, const std::string &c_imageid)
    {
      bufferlist in;
      ::encode(pspec.pool_id, in);
      ::encode(pspec.image_id, in);
      ::encode(pspec.snap_id, in);
      ::encode(c_imageid, in);
      op->exec("rbd", "remove_child", in);
    }

    int remove_child(librados::IoCtx *ioctx, const std::string &oid,
		     parent_spec pspec, const std::string &c_imageid)
    {
      librados::ObjectWriteOperation op;
      remove_child(&op, pspec, c_imageid);
      return ioctx->operate(oid, &op);
    }

    void get_children_start(librados::ObjectReadOperation *op,
                            const parent_spec &pspec) {
      bufferlist in_bl;
      ::encode(pspec.pool_id, in_bl);
      ::encode(pspec.image_id, in_bl);
      ::encode(pspec.snap_id, in_bl);
      op->exec("rbd", "get_children", in_bl);
    }

    int get_children_finish(bufferlist::iterator *it,
                            std::set<std::string>* children) {
      try {
        ::decode(*children, *it);
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }
      return 0;
    }

    int get_children(librados::IoCtx *ioctx, const std::string &oid,
		     parent_spec pspec, set<string>& children)
    {
      librados::ObjectReadOperation op;
      get_children_start(&op, pspec);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
        return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return get_children_finish(&it, &children);
    }

    void snapshot_add(librados::ObjectWriteOperation *op, snapid_t snap_id,
		      const std::string &snap_name)
    {
      bufferlist bl;
      ::encode(snap_name, bl);
      ::encode(snap_id, bl);
      op->exec("rbd", "snapshot_add", bl);
    }

    void snapshot_remove(librados::ObjectWriteOperation *op, snapid_t snap_id)
    {
      bufferlist bl;
      ::encode(snap_id, bl);
      op->exec("rbd", "snapshot_remove", bl);
    }

    void snapshot_rename(librados::ObjectWriteOperation *op,
			 snapid_t src_snap_id,
		         const std::string &dst_name)
    {
      bufferlist bl;
      ::encode(src_snap_id, bl);
      ::encode(dst_name, bl);
      op->exec("rbd", "snapshot_rename", bl);
    }

    int get_snapcontext(librados::IoCtx *ioctx, const std::string &oid,
			::SnapContext *snapc)
    {
      bufferlist inbl, outbl;

      int r = ioctx->exec(oid, "rbd", "get_snapcontext", inbl, outbl);
      if (r < 0)
	return r;

      try {
	bufferlist::iterator iter = outbl.begin();
	::decode(*snapc, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      if (!snapc->is_valid())
	return -EBADMSG;

      return 0;
    }

    void snapshot_list_start(librados::ObjectReadOperation *op,
                             const std::vector<snapid_t> &ids) {
      for (vector<snapid_t>::const_iterator it = ids.begin();
           it != ids.end(); ++it) {
        snapid_t snap_id = it->val;
        bufferlist bl1, bl2, bl3, bl4;
        ::encode(snap_id, bl1);
        op->exec("rbd", "get_snapshot_name", bl1);
        ::encode(snap_id, bl2);
        op->exec("rbd", "get_size", bl2);
        ::encode(snap_id, bl3);
        op->exec("rbd", "get_parent", bl3);
        ::encode(snap_id, bl4);
        op->exec("rbd", "get_protection_status", bl4);
      }
    }

    int snapshot_list_finish(bufferlist::iterator *it,
                             const std::vector<snapid_t> &ids,
                             std::vector<string> *names,
                             std::vector<uint64_t> *sizes,
                             std::vector<parent_info> *parents,
                             std::vector<uint8_t> *protection_statuses) {
      names->resize(ids.size());
      sizes->resize(ids.size());
      parents->resize(ids.size());
      protection_statuses->resize(ids.size());
      try {
	for (size_t i = 0; i < names->size(); ++i) {
	  uint8_t order;
	  // get_snapshot_name
	  ::decode((*names)[i], *it);
	  // get_size
	  ::decode(order, *it);
	  ::decode((*sizes)[i], *it);
	  // get_parent
	  ::decode((*parents)[i].spec.pool_id, *it);
	  ::decode((*parents)[i].spec.image_id, *it);
	  ::decode((*parents)[i].spec.snap_id, *it);
	  ::decode((*parents)[i].overlap, *it);
	  // get_protection_status
	  ::decode((*protection_statuses)[i], *it);
	}
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }
      return 0;
    }

    int snapshot_list(librados::IoCtx *ioctx, const std::string &oid,
		      const std::vector<snapid_t> &ids,
		      std::vector<string> *names,
		      std::vector<uint64_t> *sizes,
		      std::vector<parent_info> *parents,
		      std::vector<uint8_t> *protection_statuses)
    {
      librados::ObjectReadOperation op;
      snapshot_list_start(&op, ids);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
        return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return snapshot_list_finish(&it, ids, names, sizes, parents,
                                  protection_statuses);
    }

    void old_snapshot_add(librados::ObjectWriteOperation *op,
			  snapid_t snap_id, const std::string &snap_name)
    {
      bufferlist bl;
      ::encode(snap_name, bl);
      ::encode(snap_id, bl);
      op->exec("rbd", "snap_add", bl);
    }

    void old_snapshot_remove(librados::ObjectWriteOperation *op,
			     const std::string &snap_name)
    {
      bufferlist bl;
      ::encode(snap_name, bl);
      op->exec("rbd", "snap_remove", bl);
    }

    void old_snapshot_rename(librados::ObjectWriteOperation *op,
			     snapid_t src_snap_id, const std::string &dst_name)
    {
      bufferlist bl;
      ::encode(src_snap_id, bl);
      ::encode(dst_name, bl);
      op->exec("rbd", "snap_rename", bl);
    }

    void old_snapshot_list_start(librados::ObjectReadOperation *op) {
      bufferlist in_bl;
      op->exec("rbd", "snap_list", in_bl);
    }

    int old_snapshot_list_finish(bufferlist::iterator *it,
                                 std::vector<string> *names,
                                 std::vector<uint64_t> *sizes,
                                 ::SnapContext *snapc) {
      try {
	uint32_t num_snaps;
	::decode(snapc->seq, *it);
	::decode(num_snaps, *it);

	names->resize(num_snaps);
	sizes->resize(num_snaps);
	snapc->snaps.resize(num_snaps);
	for (uint32_t i = 0; i < num_snaps; ++i) {
	  ::decode(snapc->snaps[i], *it);
	  ::decode((*sizes)[i], *it);
	  ::decode((*names)[i], *it);
	}
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }
      return 0;
    }

    int old_snapshot_list(librados::IoCtx *ioctx, const std::string &oid,
			  std::vector<string> *names,
			  std::vector<uint64_t> *sizes,
			  ::SnapContext *snapc)
    {
      librados::ObjectReadOperation op;
      old_snapshot_list_start(&op);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
        return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return old_snapshot_list_finish(&it, names, sizes, snapc);
    }

    int copyup(librados::IoCtx *ioctx, const std::string &oid,
	       bufferlist data) {
      bufferlist out;
      return ioctx->exec(oid, "rbd", "copyup", data, out);
    }

    int get_protection_status(librados::IoCtx *ioctx, const std::string &oid,
			      snapid_t snap_id, uint8_t *protection_status)
    {
      bufferlist in, out;
      ::encode(snap_id.val, in);

      int r = ioctx->exec(oid, "rbd", "get_protection_status", in, out);
      if (r < 0)
	return r;

      try {
	bufferlist::iterator iter = out.begin();
	::decode(*protection_status, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      return 0;
    }

    int set_protection_status(librados::IoCtx *ioctx, const std::string &oid,
			      snapid_t snap_id, uint8_t protection_status)
    {
      // TODO remove
      librados::ObjectWriteOperation op;
      set_protection_status(&op, snap_id, protection_status);
      return ioctx->operate(oid, &op);
    }

    void set_protection_status(librados::ObjectWriteOperation *op,
                               snapid_t snap_id, uint8_t protection_status)
    {
      bufferlist in;
      ::encode(snap_id, in);
      ::encode(protection_status, in);
      op->exec("rbd", "set_protection_status", in);
    }

    void get_stripe_unit_count_start(librados::ObjectReadOperation *op) {
      bufferlist empty_bl;
      op->exec("rbd", "get_stripe_unit_count", empty_bl);
    }

    int get_stripe_unit_count_finish(bufferlist::iterator *it,
                                     uint64_t *stripe_unit,
                                     uint64_t *stripe_count) {
      assert(stripe_unit);
      assert(stripe_count);

      try {
	::decode(*stripe_unit, *it);
	::decode(*stripe_count, *it);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }
      return 0;
    }

    int get_stripe_unit_count(librados::IoCtx *ioctx, const std::string &oid,
			      uint64_t *stripe_unit, uint64_t *stripe_count)
    {
      librados::ObjectReadOperation op;
      get_stripe_unit_count_start(&op);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
        return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return get_stripe_unit_count_finish(&it, stripe_unit, stripe_count);
    }

    int set_stripe_unit_count(librados::IoCtx *ioctx, const std::string &oid,
			      uint64_t stripe_unit, uint64_t stripe_count)
    {
      bufferlist in, out;
      ::encode(stripe_unit, in);
      ::encode(stripe_count, in);
      return ioctx->exec(oid, "rbd", "set_stripe_unit_count", in, out);
    }


    /************************ rbd_id object methods ************************/

    void get_id_start(librados::ObjectReadOperation *op) {
      bufferlist empty_bl;
      op->exec("rbd", "get_id", empty_bl);
    }

    int get_id_finish(bufferlist::iterator *it, std::string *id) {
      try {
	::decode(*id, *it);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }
      return 0;
    }

    int get_id(librados::IoCtx *ioctx, const std::string &oid, std::string *id)
    {
      librados::ObjectReadOperation op;
      get_id_start(&op);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
        return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return get_id_finish(&it, id);
    }

    int set_id(librados::IoCtx *ioctx, const std::string &oid, std::string id)
    {
      bufferlist in, out;
      ::encode(id, in);
      return ioctx->exec(oid, "rbd", "set_id", in, out);
    }

    /******************** rbd_directory object methods ********************/

    int dir_get_id(librados::IoCtx *ioctx, const std::string &oid,
		   const std::string &name, std::string *id)
    {
      bufferlist in, out;
      ::encode(name, in);
      int r = ioctx->exec(oid, "rbd", "dir_get_id", in, out);
      if (r < 0)
	return r;

      bufferlist::iterator iter = out.begin();
      try {
	::decode(*id, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      return 0;
    }

    int dir_get_name(librados::IoCtx *ioctx, const std::string &oid,
		     const std::string &id, std::string *name)
    {
      bufferlist in, out;
      ::encode(id, in);
      int r = ioctx->exec(oid, "rbd", "dir_get_name", in, out);
      if (r < 0)
	return r;

      bufferlist::iterator iter = out.begin();
      try {
	::decode(*name, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      return 0;
    }

    int dir_list(librados::IoCtx *ioctx, const std::string &oid,
		 const std::string &start, uint64_t max_return,
		 map<string, string> *images)
    {
      bufferlist in, out;
      ::encode(start, in);
      ::encode(max_return, in);
      int r = ioctx->exec(oid, "rbd", "dir_list", in, out);
      if (r < 0)
	return r;

      bufferlist::iterator iter = out.begin();
      try {
	::decode(*images, iter);
      } catch (const buffer::error &err) {
	return -EBADMSG;
      }

      return 0;
    }

    int dir_add_image(librados::IoCtx *ioctx, const std::string &oid,
		      const std::string &name, const std::string &id)
    {
      bufferlist in, out;
      ::encode(name, in);
      ::encode(id, in);
      return ioctx->exec(oid, "rbd", "dir_add_image", in, out);
    }

    int dir_remove_image(librados::IoCtx *ioctx, const std::string &oid,
			 const std::string &name, const std::string &id)
    {
      bufferlist in, out;
      ::encode(name, in);
      ::encode(id, in);
      return ioctx->exec(oid, "rbd", "dir_remove_image", in, out);
    }

    void dir_rename_image(librados::ObjectWriteOperation *op,
			 const std::string &src, const std::string &dest,
			 const std::string &id)
    {
      bufferlist in;
      ::encode(src, in);
      ::encode(dest, in);
      ::encode(id, in);
      op->exec("rbd", "dir_rename_image", in);
    }

    void object_map_load_start(librados::ObjectReadOperation *op) {
      bufferlist in_bl;
      op->exec("rbd", "object_map_load", in_bl);
    }

    int object_map_load_finish(bufferlist::iterator *it,
                               ceph::BitVector<2> *object_map) {
      try {
        ::decode(*object_map, *it);
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }
      return 0;
    }

    int object_map_load(librados::IoCtx *ioctx, const std::string &oid,
			ceph::BitVector<2> *object_map)
    {
      librados::ObjectReadOperation op;
      object_map_load_start(&op);

      bufferlist out_bl;
      int r = ioctx->operate(oid, &op, &out_bl);
      if (r < 0) {
	return r;
      }

      bufferlist::iterator it = out_bl.begin();
      return object_map_load_finish(&it, object_map);
    }

    void object_map_save(librados::ObjectWriteOperation *rados_op,
                         const ceph::BitVector<2> &object_map)
    {
      ceph::BitVector<2> object_map_copy(object_map);
      object_map_copy.set_crc_enabled(false);

      bufferlist in;
      ::encode(object_map_copy, in);
      rados_op->exec("rbd", "object_map_save", in);
    }

    void object_map_resize(librados::ObjectWriteOperation *rados_op,
                           uint64_t object_count, uint8_t default_state)
    {
      bufferlist in;
      ::encode(object_count, in);
      ::encode(default_state, in);
      rados_op->exec("rbd", "object_map_resize", in);
    }

    void object_map_update(librados::ObjectWriteOperation *rados_op,
			   uint64_t start_object_no, uint64_t end_object_no,
                           uint8_t new_object_state,
			   const boost::optional<uint8_t> &current_object_state)
    {
      bufferlist in;
      ::encode(start_object_no, in);
      ::encode(end_object_no, in);
      ::encode(new_object_state, in);
      ::encode(current_object_state, in);
      rados_op->exec("rbd", "object_map_update", in);
    }

    void object_map_snap_add(librados::ObjectWriteOperation *rados_op)
    {
      bufferlist in;
      rados_op->exec("rbd", "object_map_snap_add", in);
    }

    void object_map_snap_remove(librados::ObjectWriteOperation *rados_op,
                                const ceph::BitVector<2> &object_map)
    {
      ceph::BitVector<2> object_map_copy(object_map);
      object_map_copy.set_crc_enabled(false);

      bufferlist in;
      ::encode(object_map_copy, in);
      rados_op->exec("rbd", "object_map_snap_remove", in);
    }

    int metadata_set(librados::IoCtx *ioctx, const std::string &oid,
                     const map<string, bufferlist> &data)
    {
      bufferlist in;
      ::encode(data, in);
      bufferlist out;
      return ioctx->exec(oid, "rbd", "metadata_set", in, out);
    }

    int metadata_remove(librados::IoCtx *ioctx, const std::string &oid,
                        const std::string &key)
    {
      bufferlist in;
      ::encode(key, in);
      bufferlist out;
      return ioctx->exec(oid, "rbd", "metadata_remove", in, out);
    }

    int metadata_list(librados::IoCtx *ioctx, const std::string &oid,
                      const std::string &start, uint64_t max_return,
                      map<string, bufferlist> *pairs)
    {
      assert(pairs);
      bufferlist in, out;
      ::encode(start, in);
      ::encode(max_return, in);
      int r = ioctx->exec(oid, "rbd", "metadata_list", in, out);
      if (r < 0)
        return r;

      bufferlist::iterator iter = out.begin();
      try {
        ::decode(*pairs, iter);
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }

      return 0;
    }

    int metadata_get(librados::IoCtx *ioctx, const std::string &oid,
                     const std::string &key, string *s)
    {
      assert(s);
      bufferlist in, out;
      ::encode(key, in);
      int r = ioctx->exec(oid, "rbd", "metadata_get", in, out);
      if (r < 0)
        return r;

      bufferlist::iterator iter = out.begin();
      try {
        ::decode(*s, iter);
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }

      return 0;
    }

    int mirror_is_enabled(librados::IoCtx *ioctx, bool *enabled) {
      bufferlist in_bl;
      bufferlist out_bl;
      int r = ioctx->exec(RBD_POOL_SETTINGS, "rbd", "mirror_is_enabled", in_bl,
                          out_bl);
      if (r == -ENOENT) {
        *enabled = false;
        return 0;
      } else if (r < 0) {
        return r;
      }

      try {
        bufferlist::iterator bl_it = out_bl.begin();
        ::decode(*enabled, bl_it);
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }
      return 0;
    }

    int mirror_set_enabled(librados::IoCtx *ioctx, bool enabled) {
      bufferlist in_bl;
      ::encode(enabled, in_bl);

      bufferlist out_bl;
      int r = ioctx->exec(RBD_POOL_SETTINGS, "rbd", "mirror_set_enabled", in_bl,
                          out_bl);
      if (r < 0) {
        return r;
      }
      return 0;
    }

    int mirror_peer_list(librados::IoCtx *ioctx,
                         std::vector<cls::rbd::MirrorPeer> *peers) {
      bufferlist in_bl;
      bufferlist out_bl;
      int r = ioctx->exec(RBD_POOL_SETTINGS, "rbd", "mirror_peer_list", in_bl,
                          out_bl);
      if (r < 0) {
        return r;
      }

      peers->clear();
      try {
        bufferlist::iterator bl_it = out_bl.begin();
        ::decode(*peers, bl_it);
      } catch (const buffer::error &err) {
        return -EBADMSG;
      }
      return 0;
    }

    int mirror_peer_add(librados::IoCtx *ioctx, const std::string &cluster_uuid,
                        const std::string &cluster_name,
                        const std::string &client_name) {
      cls::rbd::MirrorPeer peer(cluster_uuid, cluster_name, client_name);
      bufferlist in_bl;
      ::encode(peer, in_bl);

      bufferlist out_bl;
      int r = ioctx->exec(RBD_POOL_SETTINGS, "rbd", "mirror_peer_add", in_bl,
                          out_bl);
      if (r < 0) {
        return r;
      }
      return 0;
    }

    int mirror_peer_remove(librados::IoCtx *ioctx,
                           const std::string &cluster_uuid) {
      bufferlist in_bl;
      ::encode(cluster_uuid, in_bl);

      bufferlist out_bl;
      int r = ioctx->exec(RBD_POOL_SETTINGS, "rbd", "mirror_peer_remove", in_bl,
                          out_bl);
      if (r < 0) {
        return r;
      }
      return 0;
    }

    int mirror_peer_set_client(librados::IoCtx *ioctx,
                               const std::string &cluster_uuid,
                               const std::string &client_name) {
      bufferlist in_bl;
      ::encode(cluster_uuid, in_bl);
      ::encode(client_name, in_bl);

      bufferlist out_bl;
      int r = ioctx->exec(RBD_POOL_SETTINGS, "rbd", "mirror_peer_set_client",
                          in_bl, out_bl);
      if (r < 0) {
        return r;
      }
      return 0;
    }

    int mirror_peer_set_cluster(librados::IoCtx *ioctx,
                                const std::string &cluster_uuid,
                                const std::string &cluster_name) {
      bufferlist in_bl;
      ::encode(cluster_uuid, in_bl);
      ::encode(cluster_name, in_bl);

      bufferlist out_bl;
      int r = ioctx->exec(RBD_POOL_SETTINGS, "rbd", "mirror_peer_set_cluster",
                          in_bl, out_bl);
      if (r < 0) {
        return r;
      }
      return 0;
    }

  } // namespace cls_client
} // namespace librbd
