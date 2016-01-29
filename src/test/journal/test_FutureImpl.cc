// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "journal/FutureImpl.h"
#include "common/Cond.h"
#include "common/Finisher.h"
#include "common/Mutex.h"
#include "gtest/gtest.h"
#include "test/journal/RadosTestFixture.h"

class TestFutureImpl : public RadosTestFixture {
public:

  TestFutureImpl() : m_finisher(NULL) {
  }
  ~TestFutureImpl() {
    m_finisher->stop();
    delete m_finisher;
  }

  struct FlushHandler : public journal::FutureImpl::FlushHandler {
    uint64_t refs;
    uint64_t flushes;
    FlushHandler() : refs(0), flushes(0) {}
    virtual void get() {
      ++refs;
    }
    virtual void put() {
      assert(refs > 0);
      --refs;
    }
    virtual void flush(const journal::FutureImplPtr &future) {
      ++flushes;
    }
  };

  void SetUp() {
    RadosTestFixture::SetUp();
    m_finisher = new Finisher(reinterpret_cast<CephContext*>(m_ioctx.cct()));
    m_finisher->start();
  }

  journal::FutureImplPtr create_future(const std::string &tag, uint64_t tid,
                                       uint64_t commit_tid,
                                       const journal::FutureImplPtr &prev =
                                         journal::FutureImplPtr()) {
    journal::FutureImplPtr future(new journal::FutureImpl(*m_finisher,
                                                          tag, tid,
                                                          commit_tid));
    future->init(prev);
    return future;
  }

  void flush(const journal::FutureImplPtr &future) {
  }

  Finisher *m_finisher;

  FlushHandler m_flush_handler;
};

TEST_F(TestFutureImpl, Getters) {
  journal::FutureImplPtr future = create_future("tag", 123, 456);
  ASSERT_EQ("tag", future->get_tag());
  ASSERT_EQ(123U, future->get_tid());
  ASSERT_EQ(456U, future->get_commit_tid());
}

TEST_F(TestFutureImpl, Attach) {
  journal::FutureImplPtr future = create_future("tag", 123, 456);
  ASSERT_FALSE(future->attach(&m_flush_handler));
  ASSERT_EQ(1U, m_flush_handler.refs);
}

TEST_F(TestFutureImpl, AttachWithPendingFlush) {
  journal::FutureImplPtr future = create_future("tag", 123, 456);
  future->flush(NULL);

  ASSERT_TRUE(future->attach(&m_flush_handler));
  ASSERT_EQ(1U, m_flush_handler.refs);
}

TEST_F(TestFutureImpl, Detach) {
  journal::FutureImplPtr future = create_future("tag", 123, 456);
  ASSERT_FALSE(future->attach(&m_flush_handler));
  future->detach();
  ASSERT_EQ(0U, m_flush_handler.refs);
}

TEST_F(TestFutureImpl, DetachImplicit) {
  journal::FutureImplPtr future = create_future("tag", 123, 456);
  ASSERT_FALSE(future->attach(&m_flush_handler));
  future.reset();
  ASSERT_EQ(0U, m_flush_handler.refs);
}

TEST_F(TestFutureImpl, Flush) {
  journal::FutureImplPtr future = create_future("tag", 123, 456);
  ASSERT_FALSE(future->attach(&m_flush_handler));

  C_SaferCond cond;
  future->flush(&cond);

  ASSERT_EQ(1U, m_flush_handler.flushes);
  future->safe(-EIO);
  ASSERT_EQ(-EIO, cond.wait());
}

TEST_F(TestFutureImpl, FlushWithoutContext) {
  journal::FutureImplPtr future = create_future("tag", 123, 456);
  ASSERT_FALSE(future->attach(&m_flush_handler));

  future->flush(NULL);
  ASSERT_EQ(1U, m_flush_handler.flushes);
  future->safe(-EIO);
  ASSERT_TRUE(future->is_complete());
  ASSERT_EQ(-EIO, future->get_return_value());
}

TEST_F(TestFutureImpl, FlushChain) {
  journal::FutureImplPtr future1 = create_future("tag1", 123, 456);
  journal::FutureImplPtr future2 = create_future("tag1", 124, 457, future1);
  journal::FutureImplPtr future3 = create_future("tag2", 1, 458, future2);
  ASSERT_FALSE(future1->attach(&m_flush_handler));
  ASSERT_FALSE(future2->attach(&m_flush_handler));
  ASSERT_FALSE(future3->attach(&m_flush_handler));

  C_SaferCond cond;
  future3->flush(&cond);

  ASSERT_EQ(3U, m_flush_handler.flushes);

  future3->safe(0);
  ASSERT_FALSE(future3->is_complete());

  future1->safe(0);
  ASSERT_FALSE(future3->is_complete());

  future2->safe(-EIO);
  ASSERT_TRUE(future3->is_complete());
  ASSERT_EQ(-EIO, future3->get_return_value());
  ASSERT_EQ(-EIO, cond.wait());
  ASSERT_EQ(0, future1->get_return_value());
}

TEST_F(TestFutureImpl, FlushInProgress) {
  journal::FutureImplPtr future1 = create_future("tag1", 123, 456);
  journal::FutureImplPtr future2 = create_future("tag1", 124, 457, future1);
  ASSERT_FALSE(future1->attach(&m_flush_handler));
  ASSERT_FALSE(future2->attach(&m_flush_handler));

  future1->set_flush_in_progress();
  ASSERT_TRUE(future1->is_flush_in_progress());

  future1->flush(NULL);
  ASSERT_EQ(0U, m_flush_handler.flushes);

  future1->safe(0);
}

TEST_F(TestFutureImpl, FlushAlreadyComplete) {
  journal::FutureImplPtr future = create_future("tag1", 123, 456);
  future->safe(-EIO);

  C_SaferCond cond;
  future->flush(&cond);
  ASSERT_EQ(-EIO, cond.wait());
}

TEST_F(TestFutureImpl, Wait) {
  journal::FutureImplPtr future = create_future("tag", 1, 456);

  C_SaferCond cond;
  future->wait(&cond);
  future->safe(-EEXIST);
  ASSERT_EQ(-EEXIST, cond.wait());
}

TEST_F(TestFutureImpl, WaitAlreadyComplete) {
  journal::FutureImplPtr future = create_future("tag", 1, 456);
  future->safe(-EEXIST);

  C_SaferCond cond;
  future->wait(&cond);
  ASSERT_EQ(-EEXIST, cond.wait());
}

TEST_F(TestFutureImpl, SafePreservesError) {
  journal::FutureImplPtr future1 = create_future("tag1", 123, 456);
  journal::FutureImplPtr future2 = create_future("tag1", 124, 457, future1);

  future1->safe(-EIO);
  future2->safe(-EEXIST);
  ASSERT_TRUE(future2->is_complete());
  ASSERT_EQ(-EIO, future2->get_return_value());
}

TEST_F(TestFutureImpl, ConsistentPreservesError) {
  journal::FutureImplPtr future1 = create_future("tag1", 123, 456);
  journal::FutureImplPtr future2 = create_future("tag1", 124, 457, future1);

  future2->safe(-EEXIST);
  future1->safe(-EIO);
  ASSERT_TRUE(future2->is_complete());
  ASSERT_EQ(-EEXIST, future2->get_return_value());
}
