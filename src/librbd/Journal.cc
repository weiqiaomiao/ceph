// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/Journal.h"
#include "librbd/AioCompletion.h"
#include "librbd/AioImageRequestWQ.h"
#include "librbd/AioObjectRequest.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/journal/Entries.h"
#include "librbd/journal/Replay.h"
#include "librbd/Utils.h"
#include "journal/Journaler.h"
#include "journal/ReplayEntry.h"
#include "common/errno.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::Journal: "

namespace librbd {

using util::create_async_context_callback;
using util::create_context_callback;

namespace {

const std::string CLIENT_DESCRIPTION = "master image";

} // anonymous namespace

template <typename I>
std::ostream &operator<<(std::ostream &os,
                         const typename Journal<I>::State &state) {
  switch (state) {
  case Journal<I>::STATE_UNINITIALIZED:
    os << "Uninitialized";
    break;
  case Journal<I>::STATE_INITIALIZING:
    os << "Initializing";
    break;
  case Journal<I>::STATE_REPLAYING:
    os << "Replaying";
    break;
  case Journal<I>::STATE_FLUSHING_RESTART:
    os << "FlushingRestart";
    break;
  case Journal<I>::STATE_RESTARTING_REPLAY:
    os << "RestartingReplay";
    break;
  case Journal<I>::STATE_FLUSHING_REPLAY:
    os << "FlushingReplay";
    break;
  case Journal<I>::STATE_READY:
    os << "Ready";
    break;
  case Journal<I>::STATE_STOPPING:
    os << "Stopping";
    break;
  case Journal<I>::STATE_CLOSING:
    os << "Closing";
    break;
  case Journal<I>::STATE_CLOSED:
    os << "Closed";
    break;
  default:
    os << "Unknown (" << static_cast<uint32_t>(state) << ")";
    break;
  }
  return os;
}

template <typename I>
Journal<I>::Journal(I &image_ctx)
  : m_image_ctx(image_ctx), m_journaler(NULL),
    m_lock("Journal<I>::m_lock"), m_state(STATE_UNINITIALIZED),
    m_error_result(0), m_replay_handler(this), m_close_pending(false),
    m_event_lock("Journal<I>::m_event_lock"), m_event_tid(0),
    m_blocking_writes(false), m_journal_replay(NULL) {

  ldout(m_image_ctx.cct, 5) << this << ": ictx=" << &m_image_ctx << dendl;
}

template <typename I>
Journal<I>::~Journal() {
  assert(m_state == STATE_UNINITIALIZED || m_state == STATE_CLOSED);
  assert(m_journaler == NULL);
  assert(m_journal_replay == NULL);
  assert(m_wait_for_state_contexts.empty());
}

template <typename I>
bool Journal<I>::is_journal_supported(I &image_ctx) {
  assert(image_ctx.snap_lock.is_locked());
  return ((image_ctx.features & RBD_FEATURE_JOURNALING) &&
          !image_ctx.read_only && image_ctx.snap_id == CEPH_NOSNAP);
}

template <typename I>
int Journal<I>::create(librados::IoCtx &io_ctx, const std::string &image_id,
		       uint8_t order, uint8_t splay_width,
		       const std::string &object_pool) {
  CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
  ldout(cct, 5) << __func__ << ": image=" << image_id << dendl;

  int64_t pool_id = -1;
  if (!object_pool.empty()) {
    librados::Rados rados(io_ctx);
    IoCtx data_io_ctx;
    int r = rados.ioctx_create(object_pool.c_str(), data_io_ctx);
    if (r != 0) {
      lderr(cct) << "failed to create journal: "
		 << "error opening journal objects pool '" << object_pool
		 << "': " << cpp_strerror(r) << dendl;
      return r;
    }
    pool_id = data_io_ctx.get_id();
  }

  Journaler journaler(io_ctx, image_id, "", cct->_conf->rbd_journal_commit_age);

  int r = journaler.create(order, splay_width, pool_id);
  if (r < 0) {
    lderr(cct) << "failed to create journal: " << cpp_strerror(r) << dendl;
    return r;
  }

  r = journaler.register_client(CLIENT_DESCRIPTION);
  if (r < 0) {
    lderr(cct) << "failed to register client: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

template <typename I>
int Journal<I>::remove(librados::IoCtx &io_ctx, const std::string &image_id) {
  CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
  ldout(cct, 5) << __func__ << ": image=" << image_id << dendl;

  Journaler journaler(io_ctx, image_id, "", cct->_conf->rbd_journal_commit_age);

  bool journal_exists;
  int r = journaler.exists(&journal_exists);
  if (r < 0) {
    lderr(cct) << "failed to stat journal header: " << cpp_strerror(r) << dendl;
    return r;
  } else if (!journal_exists) {
    return 0;
  }

  C_SaferCond cond;
  journaler.init(&cond);

  r = cond.wait();
  if (r == -ENOENT) {
    return 0;
  } else if (r < 0) {
    lderr(cct) << "failed to initialize journal: " << cpp_strerror(r) << dendl;
    return r;
  }

  r = journaler.remove(false);
  if (r < 0) {
    lderr(cct) << "failed to remove journal: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

template <typename I>
int Journal<I>::reset(librados::IoCtx &io_ctx, const std::string &image_id) {
  CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
  ldout(cct, 5) << __func__ << ": image=" << image_id << dendl;

  Journaler journaler(io_ctx, image_id, "", cct->_conf->rbd_journal_commit_age);

  C_SaferCond cond;
  journaler.init(&cond);

  int r = cond.wait();
  if (r == -ENOENT) {
    return 0;
  } else if (r < 0) {
    lderr(cct) << "failed to initialize journal: " << cpp_strerror(r) << dendl;
    return r;
  }

  uint8_t order, splay_width;
  int64_t pool_id;
  journaler.get_metadata(&order, &splay_width, &pool_id);

  r = journaler.remove(true);
  if (r < 0) {
    lderr(cct) << "failed to reset journal: " << cpp_strerror(r) << dendl;
    return r;
  }
  r = journaler.create(order, splay_width, pool_id);
  if (r < 0) {
    lderr(cct) << "failed to create journal: " << cpp_strerror(r) << dendl;
    return r;
  }
  r = journaler.register_client(CLIENT_DESCRIPTION);
  if (r < 0) {
    lderr(cct) << "failed to register client: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

template <typename I>
bool Journal<I>::is_journal_ready() const {
  Mutex::Locker locker(m_lock);
  return (m_state == STATE_READY);
}

template <typename I>
bool Journal<I>::is_journal_replaying() const {
  Mutex::Locker locker(m_lock);
  return (m_state == STATE_REPLAYING ||
          m_state == STATE_FLUSHING_REPLAY ||
          m_state == STATE_RESTARTING_REPLAY);
}

template <typename I>
void Journal<I>::wait_for_journal_ready(Context *on_ready) {
  on_ready = create_async_context_callback(m_image_ctx, on_ready);

  Mutex::Locker locker(m_lock);
  if (m_state == STATE_READY) {
    on_ready->complete(m_error_result);
  } else {
    wait_for_steady_state(on_ready);
  }
}

template <typename I>
void Journal<I>::open(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  on_finish = create_async_context_callback(m_image_ctx, on_finish);

  Mutex::Locker locker(m_lock);
  assert(m_state == STATE_UNINITIALIZED);
  wait_for_steady_state(on_finish);
  create_journaler();
}

template <typename I>
void Journal<I>::close(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  on_finish = create_async_context_callback(m_image_ctx, on_finish);

  Mutex::Locker locker(m_lock);
  assert(m_state != STATE_UNINITIALIZED);
  if (m_state == STATE_CLOSED) {
    on_finish->complete(m_error_result);
    return;
  }

  if (m_state == STATE_READY) {
    stop_recording();
  }

  m_close_pending = true;
  wait_for_steady_state(on_finish);
}

template <typename I>
uint64_t Journal<I>::append_io_event(AioCompletion *aio_comp,
                                     journal::EventEntry &&event_entry,
                                     const AioObjectRequests &requests,
                                     uint64_t offset, size_t length,
                                     bool flush_entry) {
  assert(m_image_ctx.owner_lock.is_locked());

  bufferlist bl;
  ::encode(event_entry, bl);

  Future future;
  uint64_t tid;
  {
    Mutex::Locker locker(m_lock);
    assert(m_state == STATE_READY);

    Mutex::Locker event_locker(m_event_lock);
    tid = ++m_event_tid;
    assert(tid != 0);

    future = m_journaler->append("", bl);
    m_events[tid] = Event(future, aio_comp, requests, offset, length);
  }

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": "
                 << "event=" << event_entry.get_event_type() << ", "
                 << "new_reqs=" << requests.size() << ", "
                 << "offset=" << offset << ", "
                 << "length=" << length << ", "
                 << "flush=" << flush_entry << ", tid=" << tid << dendl;

  Context *on_safe = new C_IOEventSafe(this, tid);
  if (flush_entry) {
    future.flush(on_safe);
  } else {
    future.wait(on_safe);
  }
  return tid;
}

template <typename I>
void Journal<I>::commit_io_event(uint64_t tid, int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << tid << ", "
                 "r=" << r << dendl;

  Mutex::Locker event_locker(m_event_lock);
  typename Events::iterator it = m_events.find(tid);
  if (it == m_events.end()) {
    return;
  }
  complete_event(it, r);
}

template <typename I>
void Journal<I>::commit_io_event_extent(uint64_t tid, uint64_t offset,
                                        uint64_t length, int r) {
  assert(length > 0);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << tid << ", "
                 << "offset=" << offset << ", "
                 << "length=" << length << ", "
                 << "r=" << r << dendl;

  Mutex::Locker event_locker(m_event_lock);
  typename Events::iterator it = m_events.find(tid);
  if (it == m_events.end()) {
    return;
  }

  Event &event = it->second;
  if (event.ret_val == 0 && r < 0) {
    event.ret_val = r;
  }

  ExtentInterval extent;
  extent.insert(offset, length);

  ExtentInterval intersect;
  intersect.intersection_of(extent, event.pending_extents);

  event.pending_extents.subtract(intersect);
  if (!event.pending_extents.empty()) {
    ldout(cct, 20) << "pending extents: " << event.pending_extents << dendl;
    return;
  }
  complete_event(it, event.ret_val);
}

template <typename I>
void Journal<I>::append_op_event(uint64_t op_tid,
                                 journal::EventEntry &&event_entry,
                                 Context *on_safe) {
  assert(m_image_ctx.owner_lock.is_locked());

  bufferlist bl;
  ::encode(event_entry, bl);

  Future future;
  {
    Mutex::Locker locker(m_lock);
    assert(m_state == STATE_READY);
    future = m_journaler->append("", bl);
  }

  on_safe = create_async_context_callback(m_image_ctx, on_safe);
  future.flush(new C_OpEventSafe(this, op_tid, future, on_safe));

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << ": "
                 << "op_tid=" << op_tid << ", "
                 << "event=" << event_entry.get_event_type() << dendl;
}

template <typename I>
void Journal<I>::commit_op_event(uint64_t op_tid, int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << ": op_tid=" << op_tid << ", "
                 << "r=" << r << dendl;

  journal::EventEntry event_entry((journal::OpFinishEvent(op_tid, r)));

  bufferlist bl;
  ::encode(event_entry, bl);

  Future future;
  {
    Mutex::Locker locker(m_lock);
    assert(m_state == STATE_READY);
    future = m_journaler->append("", bl);
  }

  future.flush(new C_OpEventSafe(this, op_tid, future, nullptr));
}

template <typename I>
void Journal<I>::replay_op_ready(uint64_t op_tid, Context *on_resume) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << ": op_tid=" << op_tid << dendl;

  {
    Mutex::Locker locker(m_lock);
    assert(m_journal_replay != nullptr);
    m_journal_replay->replay_op_ready(op_tid, on_resume);
  }
}

template <typename I>
void Journal<I>::flush_event(uint64_t tid, Context *on_safe) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << tid << ", "
                 << "on_safe=" << on_safe << dendl;

  Future future;
  {
    Mutex::Locker event_locker(m_event_lock);
    future = wait_event(m_lock, tid, on_safe);
  }

  if (future.is_valid()) {
    future.flush(NULL);
  }
}

template <typename I>
void Journal<I>::wait_event(uint64_t tid, Context *on_safe) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << tid << ", "
                 << "on_safe=" << on_safe << dendl;

  Mutex::Locker event_locker(m_event_lock);
  wait_event(m_lock, tid, on_safe);
}

template <typename I>
typename Journal<I>::Future Journal<I>::wait_event(Mutex &lock, uint64_t tid,
                                                   Context *on_safe) {
  assert(m_event_lock.is_locked());
  CephContext *cct = m_image_ctx.cct;

  typename Events::iterator it = m_events.find(tid);
  assert(it != m_events.end());

  Event &event = it->second;
  if (event.safe) {
    // journal entry already safe
    ldout(cct, 20) << "journal entry already safe" << dendl;
    m_image_ctx.op_work_queue->queue(on_safe, event.ret_val);
    return Future();
  }

  event.on_safe_contexts.push_back(create_async_context_callback(m_image_ctx,
                                                                 on_safe));
  return event.future;
}

template <typename I>
void Journal<I>::create_journaler() {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  assert(m_lock.is_locked());
  assert(m_state == STATE_UNINITIALIZED || m_state == STATE_RESTARTING_REPLAY);
  assert(m_journaler == NULL);

  transition_state(STATE_INITIALIZING, 0);
  m_journaler = new Journaler(m_image_ctx.md_ctx, m_image_ctx.id, "",
                              m_image_ctx.journal_commit_age);
  m_journaler->init(create_async_context_callback(
    m_image_ctx, create_context_callback<
      Journal<I>, &Journal<I>::handle_initialized>(this)));
}

template <typename I>
void Journal<I>::destroy_journaler(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  assert(m_lock.is_locked());

  delete m_journal_replay;
  m_journal_replay = NULL;

  transition_state(STATE_CLOSING, r);
  m_image_ctx.op_work_queue->queue(create_context_callback<
    Journal<I>, &Journal<I>::handle_journal_destroyed>(this), 0);
}

template <typename I>
void Journal<I>::recreate_journaler(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  assert(m_lock.is_locked());
  assert(m_state == STATE_FLUSHING_RESTART ||
         m_state == STATE_FLUSHING_REPLAY);

  delete m_journal_replay;
  m_journal_replay = NULL;

  transition_state(STATE_RESTARTING_REPLAY, r);
  m_image_ctx.op_work_queue->queue(create_context_callback<
    Journal<I>, &Journal<I>::handle_journal_destroyed>(this), 0);
}

template <typename I>
void Journal<I>::complete_event(typename Events::iterator it, int r) {
  assert(m_event_lock.is_locked());
  assert(m_state == STATE_READY);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << it->first << " "
                 << "r=" << r << dendl;

  Event &event = it->second;
  if (r < 0) {
    // event recorded to journal but failed to update disk, we cannot
    // commit this IO event. this event must be replayed.
    assert(event.safe);
    lderr(cct) << "failed to commit IO to disk, replay required: "
               << cpp_strerror(r) << dendl;
  }

  event.committed_io = true;
  if (event.safe) {
    if (r >= 0) {
      m_journaler->committed(event.future);
    }
    m_events.erase(it);
  }
}

template <typename I>
void Journal<I>::handle_initialized(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  Mutex::Locker locker(m_lock);

  if (r < 0) {
    lderr(cct) << this << " " << __func__
               << "failed to initialize journal: " << cpp_strerror(r)
               << dendl;
    destroy_journaler(r);
    return;
  }

  transition_state(STATE_REPLAYING, 0);
  m_journal_replay = journal::Replay<I>::create(m_image_ctx);
  m_journaler->start_replay(&m_replay_handler);
}

template <typename I>
void Journal<I>::handle_replay_ready() {
  ReplayEntry replay_entry;
  {
    Mutex::Locker locker(m_lock);
    if (m_state != STATE_REPLAYING) {
      return;
    }

    CephContext *cct = m_image_ctx.cct;
    ldout(cct, 20) << this << " " << __func__ << dendl;
    if (!m_journaler->try_pop_front(&replay_entry)) {
      return;
    }
  }

  bufferlist data = replay_entry.get_data();
  bufferlist::iterator it = data.begin();
  Context *on_ready = create_context_callback<
    Journal<I>, &Journal<I>::handle_replay_process_ready>(this);
  Context *on_commit = new C_ReplayProcessSafe(this, std::move(replay_entry));

  m_journal_replay->process(&it, on_ready, on_commit);
}

template <typename I>
void Journal<I>::handle_replay_complete(int r) {
  CephContext *cct = m_image_ctx.cct;

  m_lock.Lock();
  if (m_state != STATE_REPLAYING) {
    m_lock.Unlock();
    return;
  }

  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;
  m_journaler->stop_replay();
  if (r < 0) {
    transition_state(STATE_FLUSHING_RESTART, r);
    m_lock.Unlock();

    m_journal_replay->flush(create_context_callback<
      Journal<I>, &Journal<I>::handle_flushing_restart>(this));
  } else {
    transition_state(STATE_FLUSHING_REPLAY, 0);
    m_lock.Unlock();

    m_journal_replay->flush(create_context_callback<
      Journal<I>, &Journal<I>::handle_flushing_replay>(this));
  }
}

template <typename I>
void Journal<I>::handle_replay_process_ready(int r) {
  // journal::Replay is ready for more events -- attempt to pop another
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  assert(r == 0);
  handle_replay_ready();
}

template <typename I>
void Journal<I>::handle_replay_process_safe(ReplayEntry replay_entry, int r) {
  Mutex::Locker locker(m_lock);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;
  if (r < 0) {
    lderr(cct) << "failed to commit journal event to disk: " << cpp_strerror(r)
               << dendl;

    if (m_state == STATE_REPLAYING) {
      // abort the replay if we have an error
      m_journaler->stop_replay();
      transition_state(STATE_FLUSHING_RESTART, r);

      m_journal_replay->flush(create_context_callback<
        Journal<I>, &Journal<I>::handle_flushing_restart>(this));
      return;
    } else if (m_state == STATE_FLUSHING_REPLAY) {
      // end-of-replay flush in-progress -- we need to restart replay
      transition_state(STATE_FLUSHING_RESTART, r);
      return;
    }
  } else {
    // only commit the entry if written successfully
    m_journaler->committed(replay_entry);
  }
}

template <typename I>
void Journal<I>::handle_flushing_restart(int r) {
  Mutex::Locker locker(m_lock);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  assert(r == 0);
  assert(m_state == STATE_FLUSHING_RESTART);
  if (m_close_pending) {
    destroy_journaler(r);
    return;
  }

  recreate_journaler(r);
}

template <typename I>
void Journal<I>::handle_flushing_replay(int r) {
  Mutex::Locker locker(m_lock);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  assert(r == 0);
  assert(m_state == STATE_FLUSHING_REPLAY || m_state == STATE_FLUSHING_RESTART);
  if (m_close_pending) {
    destroy_journaler(r);
    return;
  } else if (m_state == STATE_FLUSHING_RESTART) {
    // failed to replay one-or-more events -- restart
    recreate_journaler(0);
    return;
  }

  delete m_journal_replay;
  m_journal_replay = NULL;

  m_error_result = 0;
  m_journaler->start_append(m_image_ctx.journal_object_flush_interval,
			    m_image_ctx.journal_object_flush_bytes,
			    m_image_ctx.journal_object_flush_age);
  transition_state(STATE_READY, 0);
}

template <typename I>
void Journal<I>::handle_recording_stopped(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  Mutex::Locker locker(m_lock);
  assert(m_state == STATE_STOPPING);

  destroy_journaler(r);
}

template <typename I>
void Journal<I>::handle_journal_destroyed(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << this << " " << __func__
               << "error detected while closing journal: " << cpp_strerror(r)
               << dendl;
  }

  Mutex::Locker locker(m_lock);
  delete m_journaler;
  m_journaler = nullptr;

  assert(m_state == STATE_CLOSING || m_state == STATE_RESTARTING_REPLAY);
  if (m_state == STATE_RESTARTING_REPLAY) {
    create_journaler();
    return;
  }

  transition_state(STATE_CLOSED, r);
}

template <typename I>
void Journal<I>::handle_io_event_safe(int r, uint64_t tid) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << ", "
                 << "tid=" << tid << dendl;

  // journal will be flushed before closing
  assert(m_state == STATE_READY || m_state == STATE_STOPPING);
  if (r < 0) {
    lderr(cct) << "failed to commit IO event: "  << cpp_strerror(r) << dendl;
  }

  AioCompletion *aio_comp;
  AioObjectRequests aio_object_requests;
  Contexts on_safe_contexts;
  {
    Mutex::Locker event_locker(m_event_lock);
    typename Events::iterator it = m_events.find(tid);
    assert(it != m_events.end());

    Event &event = it->second;
    aio_comp = event.aio_comp;
    aio_object_requests.swap(event.aio_object_requests);
    on_safe_contexts.swap(event.on_safe_contexts);

    if (r < 0 || event.committed_io) {
      // failed journal write so IO won't be sent -- or IO extent was
      // overwritten by future IO operations so this was a no-op IO event
      event.ret_val = r;
      m_journaler->committed(event.future);
    }

    if (event.committed_io) {
      m_events.erase(it);
    } else {
      event.safe = true;
    }
  }

  ldout(cct, 20) << "completing tid=" << tid << dendl;

  if (r < 0) {
    // don't send aio requests if the journal fails -- bubble error up
    aio_comp->fail(cct, r);
  } else {
    // send any waiting aio requests now that journal entry is safe
    RWLock::RLocker owner_locker(m_image_ctx.owner_lock);
    for (AioObjectRequests::iterator it = aio_object_requests.begin();
         it != aio_object_requests.end(); ++it) {
      (*it)->send();
    }
  }

  // alert the cache about the journal event status
  for (Contexts::iterator it = on_safe_contexts.begin();
       it != on_safe_contexts.end(); ++it) {
    (*it)->complete(r);
  }
}

template <typename I>
void Journal<I>::handle_op_event_safe(int r, uint64_t tid, const Future &future,
                                      Context *on_safe) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << ", "
                 << "tid=" << tid << dendl;

  // journal will be flushed before closing
  assert(m_state == STATE_READY || m_state == STATE_STOPPING);
  if (r < 0) {
    lderr(cct) << "failed to commit op event: "  << cpp_strerror(r) << dendl;
  }

  m_journaler->committed(future);
  if (on_safe != nullptr) {
    on_safe->complete(r);
  }
}

template <typename I>
void Journal<I>::stop_recording() {
  assert(m_lock.is_locked());
  assert(m_journaler != NULL);

  assert(m_state == STATE_READY);
  transition_state(STATE_STOPPING, 0);

  m_journaler->stop_append(util::create_async_context_callback(
    m_image_ctx, create_context_callback<
      Journal<I>, &Journal<I>::handle_recording_stopped>(this)));
}

template <typename I>
void Journal<I>::transition_state(State state, int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": new state=" << state << dendl;
  assert(m_lock.is_locked());
  m_state = state;

  if (m_error_result == 0 && r < 0) {
    m_error_result = r;
  }

  if (is_steady_state()) {
    Contexts wait_for_state_contexts(std::move(m_wait_for_state_contexts));
    for (auto ctx : wait_for_state_contexts) {
      ctx->complete(m_error_result);
    }
  }
}

template <typename I>
bool Journal<I>::is_steady_state() const {
  assert(m_lock.is_locked());
  switch (m_state) {
  case STATE_READY:
  case STATE_CLOSED:
    return true;
  case STATE_UNINITIALIZED:
  case STATE_INITIALIZING:
  case STATE_REPLAYING:
  case STATE_FLUSHING_RESTART:
  case STATE_RESTARTING_REPLAY:
  case STATE_FLUSHING_REPLAY:
  case STATE_STOPPING:
  case STATE_CLOSING:
    break;
  }
  return false;
}

template <typename I>
void Journal<I>::wait_for_steady_state(Context *on_state) {
  assert(m_lock.is_locked());
  assert(!is_steady_state());

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": on_state=" << on_state
                 << dendl;
  m_wait_for_state_contexts.push_back(on_state);
}

} // namespace librbd

template class librbd::Journal<librbd::ImageCtx>;
