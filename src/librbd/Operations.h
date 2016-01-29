// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_OPERATIONS_H
#define CEPH_LIBRBD_OPERATIONS_H

#include "include/int_types.h"
#include "include/atomic.h"
#include <string>
#include <boost/function.hpp>

class Context;

namespace librbd {

class ImageCtx;
class ProgressContext;

template <typename ImageCtxT = ImageCtx>
class Operations {
public:
  Operations(ImageCtxT &image_ctx);

  int flatten(ProgressContext &prog_ctx);
  void flatten(ProgressContext &prog_ctx, Context *on_finish);

  int rebuild_object_map(ProgressContext &prog_ctx);
  void rebuild_object_map(ProgressContext &prog_ctx, Context *on_finish);

  int rename(const char *dstname);
  void rename(const char *dstname, Context *on_finish);

  int resize(uint64_t size, ProgressContext& prog_ctx);
  void resize(uint64_t size, ProgressContext &prog_ctx, Context *on_finish,
              uint64_t journal_op_tid);

  int snap_create(const char *snap_name);
  void snap_create(const char *snap_name, Context *on_finish,
                   uint64_t journal_op_tid);

  int snap_rollback(const char *snap_name, ProgressContext& prog_ctx);
  void snap_rollback(const char *snap_name, ProgressContext& prog_ctx,
                     Context *on_finish);

  int snap_remove(const char *snap_name);
  void snap_remove(const char *snap_name, Context *on_finish);

  int snap_rename(const char *srcname, const char *dstname);
  void snap_rename(const uint64_t src_snap_id, const char *dst_name,
                   Context *on_finish);

  int snap_protect(const char *snap_name);
  void snap_protect(const char *snap_name, Context *on_finish);

  int snap_unprotect(const char *snap_name);
  void snap_unprotect(const char *snap_name, Context *on_finish);

  int prepare_image_update();

private:
  ImageCtxT &m_image_ctx;
  atomic_t m_async_request_seq;

  int invoke_async_request(const std::string& request_type,
                           bool permit_snapshot,
                           const boost::function<void(Context*)>& local_request,
                           const boost::function<int()>& remote_request);
  void notify_change();
};

} // namespace librbd

extern template class librbd::Operations<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_OPERATIONS_H
