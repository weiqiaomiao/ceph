// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "journal/JournalMetadata.h"
#include "journal/Utils.h"
#include "common/errno.h"
#include "common/Finisher.h"
#include "common/Timer.h"
#include "cls/journal/cls_journal_client.h"
#include <set>

#define dout_subsys ceph_subsys_journaler
#undef dout_prefix
#define dout_prefix *_dout << "JournalMetadata: "

namespace journal {

using namespace cls::journal;

JournalMetadata::JournalMetadata(librados::IoCtx &ioctx,
                                 const std::string &oid,
                                 const std::string &client_id,
                                 double commit_interval)
    : RefCountedObject(NULL, 0), m_cct(NULL), m_oid(oid),
      m_client_id(client_id), m_commit_interval(commit_interval), m_order(0),
      m_splay_width(0), m_pool_id(-1), m_initialized(false), m_finisher(NULL),
      m_timer(NULL), m_timer_lock("JournalMetadata::m_timer_lock"),
      m_lock("JournalMetadata::m_lock"), m_commit_tid(0), m_watch_ctx(this),
      m_watch_handle(0), m_minimum_set(0), m_active_set(0),
      m_update_notifications(0), m_commit_position_ctx(NULL),
      m_commit_position_task_ctx(NULL) {
  m_ioctx.dup(ioctx);
  m_cct = reinterpret_cast<CephContext*>(m_ioctx.cct());
}

JournalMetadata::~JournalMetadata() {
  if (m_initialized) {
    shutdown();
  }
}

void JournalMetadata::init(Context *on_init) {
  assert(!m_initialized);
  m_initialized = true;

  m_finisher = new Finisher(m_cct);
  m_finisher->start();

  m_timer = new SafeTimer(m_cct, m_timer_lock, true);
  m_timer->init();

  int r = m_ioctx.watch2(m_oid, &m_watch_handle, &m_watch_ctx);
  if (r < 0) {
    lderr(m_cct) << __func__ << ": failed to watch journal"
                 << cpp_strerror(r) << dendl;
    on_init->complete(r);
    return;
  }

  C_ImmutableMetadata *ctx = new C_ImmutableMetadata(this, on_init);
  client::get_immutable_metadata(m_ioctx, m_oid, &m_order, &m_splay_width,
                                 &m_pool_id, ctx);
}

void JournalMetadata::shutdown() {
  assert(m_initialized);
  {
    Mutex::Locker locker(m_lock);
    m_initialized = false;

    if (m_watch_handle != 0) {
      m_ioctx.unwatch2(m_watch_handle);
      m_watch_handle = 0;
    }
  }

  flush_commit_position();

  if (m_timer != NULL) {
    Mutex::Locker locker(m_timer_lock);
    m_timer->shutdown();
    delete m_timer;
    m_timer = NULL;
  }

  if (m_finisher != NULL) {
    m_finisher->stop();
    delete m_finisher;
    m_finisher = NULL;
  }

  librados::Rados rados(m_ioctx);
  rados.watch_flush();

  m_async_op_tracker.wait_for_ops();
  m_ioctx.aio_flush();
}

int JournalMetadata::register_client(const std::string &description) {
  ldout(m_cct, 10) << __func__ << ": " << m_client_id << dendl;
  int r = client::client_register(m_ioctx, m_oid, m_client_id, description);
  if (r < 0) {
    lderr(m_cct) << "failed to register journal client '" << m_client_id
                 << "': " << cpp_strerror(r) << dendl;
    return r;
  }

  notify_update();
  return 0;
}

int JournalMetadata::unregister_client() {
  assert(!m_client_id.empty());

  ldout(m_cct, 10) << __func__ << ": " << m_client_id << dendl;
  int r = client::client_unregister(m_ioctx, m_oid, m_client_id);
  if (r < 0) {
    lderr(m_cct) << "failed to unregister journal client '" << m_client_id
                 << "': " << cpp_strerror(r) << dendl;
    return r;
  }

  notify_update();
  return 0;
}

void JournalMetadata::add_listener(Listener *listener) {
  Mutex::Locker locker(m_lock);
  while (m_update_notifications > 0) {
    m_update_cond.Wait(m_lock);
  }
  m_listeners.push_back(listener);
}

void JournalMetadata::remove_listener(Listener *listener) {
  Mutex::Locker locker(m_lock);
  while (m_update_notifications > 0) {
    m_update_cond.Wait(m_lock);
  }
  m_listeners.remove(listener);
}

void JournalMetadata::set_minimum_set(uint64_t object_set) {
  Mutex::Locker locker(m_lock);

  ldout(m_cct, 20) << __func__ << ": current=" << m_minimum_set
                   << ", new=" << object_set << dendl;
  if (m_minimum_set >= object_set) {
    return;
  }

  librados::ObjectWriteOperation op;
  client::set_minimum_set(&op, object_set);

  C_NotifyUpdate *ctx = new C_NotifyUpdate(this);
  librados::AioCompletion *comp =
    librados::Rados::aio_create_completion(ctx, NULL,
                                           utils::rados_ctx_callback);
  int r = m_ioctx.aio_operate(m_oid, comp, &op);
  assert(r == 0);
  comp->release();

  m_minimum_set = object_set;
}

void JournalMetadata::set_active_set(uint64_t object_set) {
  Mutex::Locker locker(m_lock);

  ldout(m_cct, 20) << __func__ << ": current=" << m_active_set
                   << ", new=" << object_set << dendl;
  if (m_active_set >= object_set) {
    return;
  }

  librados::ObjectWriteOperation op;
  client::set_active_set(&op, object_set);

  C_NotifyUpdate *ctx = new C_NotifyUpdate(this);
  librados::AioCompletion *comp =
    librados::Rados::aio_create_completion(ctx, NULL,
                                           utils::rados_ctx_callback);
  int r = m_ioctx.aio_operate(m_oid, comp, &op);
  assert(r == 0);
  comp->release();

  m_active_set = object_set;
}

void JournalMetadata::flush_commit_position() {
  {
    Mutex::Locker timer_locker(m_timer_lock);
    Mutex::Locker locker(m_lock);
    if (m_commit_position_task_ctx == NULL) {
      return;
    }

    m_timer->cancel_event(m_commit_position_task_ctx);
    m_commit_position_task_ctx = NULL;
  }
  handle_commit_position_task();
}

void JournalMetadata::set_commit_position(
    const ObjectSetPosition &commit_position, Context *on_safe) {
  assert(on_safe != NULL);

  Context *stale_ctx = nullptr;
  {
    Mutex::Locker timer_locker(m_timer_lock);
    Mutex::Locker locker(m_lock);
    ldout(m_cct, 20) << __func__ << ": current=" << m_client.commit_position
                     << ", new=" << commit_position << dendl;
    if (commit_position <= m_client.commit_position ||
        commit_position <= m_commit_position) {
      stale_ctx = on_safe;
    } else {
      stale_ctx = m_commit_position_ctx;

      m_client.commit_position = commit_position;
      m_commit_position = commit_position;
      m_commit_position_ctx = on_safe;
      schedule_commit_task();
    }
  }

  if (stale_ctx != nullptr) {
    stale_ctx->complete(-ESTALE);
  }
}

void JournalMetadata::reserve_tid(const std::string &tag, uint64_t tid) {
  Mutex::Locker locker(m_lock);
  uint64_t &allocated_tid = m_allocated_tids[tag];
  if (allocated_tid <= tid) {
    allocated_tid = tid + 1;
  }
}

bool JournalMetadata::get_last_allocated_tid(const std::string &tag,
                                             uint64_t *tid) const {
  Mutex::Locker locker(m_lock);

  AllocatedTids::const_iterator it = m_allocated_tids.find(tag);
  if (it == m_allocated_tids.end()) {
    return false;
  }

  assert(it->second > 0);
  *tid = it->second - 1;
  return true;
}

void JournalMetadata::handle_immutable_metadata(int r, Context *on_init) {
  if (r < 0) {
    lderr(m_cct) << "failed to initialize immutable metadata: "
                 << cpp_strerror(r) << dendl;
    on_init->complete(r);
    return;
  }

  ldout(m_cct, 10) << "initialized immutable metadata" << dendl;
  refresh(on_init);
}

void JournalMetadata::refresh(Context *on_complete) {
  ldout(m_cct, 10) << "refreshing mutable metadata" << dendl;
  C_Refresh *refresh = new C_Refresh(this, on_complete);
  client::get_mutable_metadata(m_ioctx, m_oid, &refresh->minimum_set,
                               &refresh->active_set,
                               &refresh->registered_clients, refresh);
}

void JournalMetadata::handle_refresh_complete(C_Refresh *refresh, int r) {
  ldout(m_cct, 10) << "refreshed mutable metadata: r=" << r << dendl;
  if (r == 0) {
    Mutex::Locker locker(m_lock);

    Client client(m_client_id, "");
    RegisteredClients::iterator it = refresh->registered_clients.find(client);
    if (it != refresh->registered_clients.end()) {
      m_minimum_set = refresh->minimum_set;
      m_active_set = refresh->active_set;
      m_registered_clients = refresh->registered_clients;
      m_client = *it;

      ++m_update_notifications;
      m_lock.Unlock();
      for (Listeners::iterator it = m_listeners.begin();
           it != m_listeners.end(); ++it) {
        (*it)->handle_update(this);
      }
      m_lock.Lock();
      if (--m_update_notifications == 0) {
        m_update_cond.Signal();
      }
    } else {
      lderr(m_cct) << "failed to locate client: " << m_client_id << dendl;
      r = -ENOENT;
    }
  }

  if (refresh->on_finish != NULL) {
    refresh->on_finish->complete(r);
  }
}

void JournalMetadata::schedule_commit_task() {
  assert(m_timer_lock.is_locked());
  assert(m_lock.is_locked());

  if (m_commit_position_task_ctx == NULL) {
    m_commit_position_task_ctx = new C_CommitPositionTask(this);
    m_timer->add_event_after(m_commit_interval, m_commit_position_task_ctx);
  }
}

void JournalMetadata::handle_commit_position_task() {
  Mutex::Locker locker(m_lock);

  librados::ObjectWriteOperation op;
  client::client_commit(&op, m_client_id, m_commit_position);

  C_NotifyUpdate *ctx = new C_NotifyUpdate(this, m_commit_position_ctx);
  m_commit_position_ctx = NULL;

  librados::AioCompletion *comp =
    librados::Rados::aio_create_completion(ctx, NULL,
                                           utils::rados_ctx_callback);
  int r = m_ioctx.aio_operate(m_oid, comp, &op);
  assert(r == 0);
  comp->release();
}

void JournalMetadata::schedule_watch_reset() {
  assert(m_timer_lock.is_locked());
  m_timer->add_event_after(0.1, new C_WatchReset(this));
}

void JournalMetadata::handle_watch_reset() {
  assert(m_timer_lock.is_locked());
  if (!m_initialized) {
    return;
  }

  int r = m_ioctx.watch2(m_oid, &m_watch_handle, &m_watch_ctx);
  if (r < 0) {
    lderr(m_cct) << __func__ << ": failed to watch journal"
                 << cpp_strerror(r) << dendl;
    schedule_watch_reset();
  } else {
    ldout(m_cct, 10) << __func__ << ": reset journal watch" << dendl;
    refresh(NULL);
  }
}

void JournalMetadata::handle_watch_notify(uint64_t notify_id, uint64_t cookie) {
  ldout(m_cct, 10) << "journal header updated" << dendl;

  bufferlist bl;
  m_ioctx.notify_ack(m_oid, notify_id, cookie, bl);

  refresh(NULL);
}

void JournalMetadata::handle_watch_error(int err) {
  lderr(m_cct) << "journal watch error: " << cpp_strerror(err) << dendl;
  Mutex::Locker timer_locker(m_timer_lock);
  Mutex::Locker locker(m_lock);

  // release old watch on error
  if (m_watch_handle != 0) {
    m_ioctx.unwatch2(m_watch_handle);
    m_watch_handle = 0;
  }

  if (m_initialized && err != -ENOENT) {
    schedule_watch_reset();
  }
}

uint64_t JournalMetadata::allocate_commit_tid(uint64_t object_num,
                                              const std::string &tag,
                                              uint64_t tid) {
  Mutex::Locker locker(m_lock);
  uint64_t commit_tid = ++m_commit_tid;
  m_pending_commit_tids[commit_tid] = CommitEntry(object_num, tag, tid);

  ldout(m_cct, 20) << "allocated commit tid: commit_tid=" << commit_tid << " ["
                   << "object_num=" << object_num << ", "
                   << "tag=" << tag << ", tid=" << tid << "]" << dendl;
  return commit_tid;
}

bool JournalMetadata::committed(uint64_t commit_tid,
                                ObjectSetPosition *object_set_position) {
  ldout(m_cct, 20) << "committed tid=" << commit_tid << dendl;

  Mutex::Locker locker(m_lock);
  {
    CommitTids::iterator it = m_pending_commit_tids.find(commit_tid);
    assert(it != m_pending_commit_tids.end());

    CommitEntry &commit_entry = it->second;
    commit_entry.committed = true;
  }

  if (!m_commit_position.entry_positions.empty()) {
    *object_set_position = m_commit_position;
  } else {
    *object_set_position = m_client.commit_position;
  }

  bool update_commit_position = false;
  while (!m_pending_commit_tids.empty()) {
    CommitTids::iterator it = m_pending_commit_tids.begin();
    CommitEntry &commit_entry = it->second;
    if (!commit_entry.committed) {
      break;
    }

    object_set_position->object_number = commit_entry.object_num;
    if (!object_set_position->entry_positions.empty() &&
        object_set_position->entry_positions.front().tag == commit_entry.tag) {
      object_set_position->entry_positions.front() = EntryPosition(
        commit_entry.tag, commit_entry.tid);
    } else {
      object_set_position->entry_positions.push_front(EntryPosition(
        commit_entry.tag, commit_entry.tid));
    }
    m_pending_commit_tids.erase(it);
    update_commit_position = true;
  }

  if (update_commit_position) {
    // prune the position to have unique tags in commit-order
    std::set<std::string> in_use_tags;
    EntryPositions::iterator it = object_set_position->entry_positions.begin();
    while (it != object_set_position->entry_positions.end()) {
      if (!in_use_tags.insert(it->tag).second) {
        it = object_set_position->entry_positions.erase(it);
      } else {
        ++it;
      }
    }

    ldout(m_cct, 20) << "updated object set position: " << *object_set_position
                     << dendl;
  }
  return update_commit_position;
}

void JournalMetadata::notify_update() {
  ldout(m_cct, 10) << "notifying journal header update" << dendl;

  bufferlist bl;
  m_ioctx.notify2(m_oid, bl, 5000, NULL);
}

void JournalMetadata::async_notify_update() {
  ldout(m_cct, 10) << "async notifying journal header update" << dendl;

  C_AioNotify *ctx = new C_AioNotify(this);
  librados::AioCompletion *comp =
    librados::Rados::aio_create_completion(ctx, NULL,
                                           utils::rados_ctx_callback);

  bufferlist bl;
  int r = m_ioctx.aio_notify(m_oid, comp, bl, 5000, NULL);
  assert(r == 0);

  comp->release();
}

void JournalMetadata::handle_notified(int r) {
  ldout(m_cct, 10) << "notified journal header update: r=" << r << dendl;
}

std::ostream &operator<<(std::ostream &os,
			 const JournalMetadata::RegisteredClients &clients) {
  os << "[";
  for (JournalMetadata::RegisteredClients::const_iterator c = clients.begin();
       c != clients.end(); c++) {
    os << (c == clients.begin() ? "" : ", " ) << *c;
  }
  os << "]";
  return os;
}

std::ostream &operator<<(std::ostream &os,
			 const JournalMetadata &jm) {
  Mutex::Locker locker(jm.m_lock);
  os << "[oid=" << jm.m_oid << ", "
     << "initialized=" << jm.m_initialized << ", "
     << "order=" << (int)jm.m_order << ", "
     << "splay_width=" << (int)jm.m_splay_width << ", "
     << "pool_id=" << jm.m_pool_id << ", "
     << "minimum_set=" << jm.m_minimum_set << ", "
     << "active_set=" << jm.m_active_set << ", "
     << "client_id=" << jm.m_client_id << ", "
     << "commit_tid=" << jm.m_commit_tid << ", "
     << "commit_interval=" << jm.m_commit_interval << ", "
     << "commit_position=" << jm.m_commit_position << ", "
     << "registered_clients=" << jm.m_registered_clients << "]";
  return os;
}

} // namespace journal
