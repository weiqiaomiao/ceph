// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "journal/Journaler.h"
#include "include/stringify.h"
#include "common/errno.h"
#include "journal/Entry.h"
#include "journal/FutureImpl.h"
#include "journal/JournalMetadata.h"
#include "journal/JournalPlayer.h"
#include "journal/JournalRecorder.h"
#include "journal/JournalTrimmer.h"
#include "journal/ReplayEntry.h"
#include "journal/ReplayHandler.h"
#include "cls/journal/cls_journal_client.h"
#include "cls/journal/cls_journal_types.h"

#define dout_subsys ceph_subsys_journaler
#undef dout_prefix
#define dout_prefix *_dout << "Journaler: "

namespace journal {

namespace {

static const std::string JOURNAL_HEADER_PREFIX = "journal.";
static const std::string JOURNAL_OBJECT_PREFIX = "journal_data.";

struct C_DeleteRecorder : public Context {
  JournalRecorder *recorder;
  Context *on_safe;
  C_DeleteRecorder(JournalRecorder *_recorder, Context *_on_safe)
    : recorder(_recorder), on_safe(_on_safe) {
  }
  virtual void finish(int r) {
    delete recorder;
    on_safe->complete(r);
  }
};

} // anonymous namespace

using namespace cls::journal;

std::string Journaler::header_oid(const std::string &journal_id) {
  return JOURNAL_HEADER_PREFIX + journal_id;
}

std::string Journaler::object_oid_prefix(int pool_id,
					 const std::string &journal_id) {
  return JOURNAL_OBJECT_PREFIX + stringify(pool_id) + "." + journal_id + ".";
}

Journaler::Journaler(librados::IoCtx &header_ioctx,
		     const std::string &journal_id,
		     const std::string &client_id, double commit_interval)
  : m_client_id(client_id), m_metadata(NULL), m_player(NULL), m_recorder(NULL),
    m_trimmer(NULL)
{
  m_header_ioctx.dup(header_ioctx);
  m_cct = reinterpret_cast<CephContext *>(m_header_ioctx.cct());

  m_header_oid = header_oid(journal_id);
  m_object_oid_prefix = object_oid_prefix(m_header_ioctx.get_id(), journal_id);

  m_metadata = new JournalMetadata(m_header_ioctx, m_header_oid, m_client_id,
                                   commit_interval);
  m_metadata->get();
}

Journaler::~Journaler() {
  if (m_metadata != NULL) {
    m_metadata->put();
    m_metadata = NULL;
  }
  delete m_trimmer;
  assert(m_player == NULL);
  assert(m_recorder == NULL);
}

int Journaler::exists(bool *header_exists) const {
  int r = m_header_ioctx.stat(m_header_oid, NULL, NULL);
  if (r < 0 && r != -ENOENT) {
    return r;
  }

  *header_exists = (r == 0);
  return 0;
}

void Journaler::init(Context *on_init) {
  m_metadata->init(new C_InitJournaler(this, on_init));
}

int Journaler::init_complete() {
  int64_t pool_id = m_metadata->get_pool_id();

  if (pool_id < 0 || pool_id == m_header_ioctx.get_id()) {
    ldout(m_cct, 20) << "using image pool for journal data" << dendl;
    m_data_ioctx.dup(m_header_ioctx);
  } else {
    ldout(m_cct, 20) << "using pool id=" << pool_id << " for journal data"
		     << dendl;
    librados::Rados rados(m_header_ioctx);
    int r = rados.ioctx_create2(pool_id, m_data_ioctx);
    if (r < 0) {
      if (r == -ENOENT) {
	ldout(m_cct, 1) << "pool id=" << pool_id << " no longer exists"
			<< dendl;
      }
      return r;
    }
  }
  m_trimmer = new JournalTrimmer(m_data_ioctx, m_object_oid_prefix,
                                 m_metadata);
  return 0;
}

void Journaler::shutdown() {
  m_metadata->shutdown();
}

int Journaler::create(uint8_t order, uint8_t splay_width, int64_t pool_id) {
  if (order > 64 || order < 12) {
    lderr(m_cct) << "order must be in the range [12, 64]" << dendl;
    return -EDOM;
  }
  if (splay_width == 0) {
    return -EINVAL;
  }

  ldout(m_cct, 5) << "creating new journal: " << m_header_oid << dendl;
  int r = client::create(m_header_ioctx, m_header_oid, order, splay_width,
			 pool_id);
  if (r < 0) {
    lderr(m_cct) << "failed to create journal: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

int Journaler::remove(bool force) {
  m_metadata->shutdown();

  ldout(m_cct, 5) << "removing journal: " << m_header_oid << dendl;
  int r = m_trimmer->remove_objects(force);
  if (r < 0) {
    lderr(m_cct) << "failed to remove journal objects: " << cpp_strerror(r)
                 << dendl;
    return r;
  }

  r = m_header_ioctx.remove(m_header_oid);
  if (r < 0) {
    lderr(m_cct) << "failed to remove journal header: " << cpp_strerror(r)
                 << dendl;
    return r;
  }
  return 0;
}

int Journaler::register_client(const std::string &description) {
  return m_metadata->register_client(description);
}

int Journaler::unregister_client() {
  return m_metadata->unregister_client();
}

void Journaler::start_replay(ReplayHandler *replay_handler) {
  create_player(replay_handler);
  m_player->prefetch();
}

void Journaler::start_live_replay(ReplayHandler *replay_handler,
                                  double interval) {
  create_player(replay_handler);
  m_player->prefetch_and_watch(interval);
}

bool Journaler::try_pop_front(ReplayEntry *replay_entry,
			      std::string* tag) {
  assert(m_player != NULL);

  Entry entry;
  uint64_t commit_tid;
  if (!m_player->try_pop_front(&entry, &commit_tid)) {
    return false;
  }

  *replay_entry = ReplayEntry(entry.get_data(), commit_tid);
  if (tag != NULL) {
    *tag = entry.get_tag();
  }
  return true;
}

void Journaler::stop_replay() {
  assert(m_player != NULL);
  m_player->unwatch();
  delete m_player;
  m_player = NULL;
}

void Journaler::committed(const ReplayEntry &replay_entry) {
  m_trimmer->committed(replay_entry.get_commit_tid());
}

void Journaler::committed(const Future &future) {
  FutureImplPtr future_impl = future.get_future_impl();
  m_trimmer->committed(future_impl->get_commit_tid());
}

void Journaler::start_append(int flush_interval, uint64_t flush_bytes,
			     double flush_age) {
  assert(m_recorder == NULL);

  // TODO verify active object set >= current replay object set

  m_recorder = new JournalRecorder(m_data_ioctx, m_object_oid_prefix,
				   m_metadata, flush_interval, flush_bytes,
				   flush_age);
}

void Journaler::stop_append(Context *on_safe) {
  assert(m_recorder != NULL);

  flush(new C_DeleteRecorder(m_recorder, on_safe));
  m_recorder = NULL;
}

Future Journaler::append(const std::string &tag, const bufferlist &payload_bl) {
  return m_recorder->append(tag, payload_bl);
}

void Journaler::flush(Context *on_safe) {
  m_recorder->flush(on_safe);
}

void Journaler::create_player(ReplayHandler *replay_handler) {
  assert(m_player == NULL);
  m_player = new JournalPlayer(m_data_ioctx, m_object_oid_prefix, m_metadata,
                               replay_handler);
}

void Journaler::get_metadata(uint8_t *order, uint8_t *splay_width,
			     int64_t *pool_id) {
  assert(m_metadata != NULL);

  *order = m_metadata->get_order();
  *splay_width = m_metadata->get_splay_width();
  *pool_id = m_metadata->get_pool_id();
}

std::ostream &operator<<(std::ostream &os,
			 const Journaler &journaler) {
  os << "[metadata=";
  if (journaler.m_metadata != NULL) {
    os << *journaler.m_metadata;
  } else {
    os << "NULL";
  }
  os << "]";
  return os;
}

} // namespace journal
