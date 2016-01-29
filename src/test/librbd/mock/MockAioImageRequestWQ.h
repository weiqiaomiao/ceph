// -*- mode:C; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_TEST_LIBRBD_MOCK_AIO_IMAGE_REQUEST_WQ_H
#define CEPH_TEST_LIBRBD_MOCK_AIO_IMAGE_REQUEST_WQ_H

#include "gmock/gmock.h"

namespace librbd {

struct MockAioImageRequestWQ {
  MOCK_METHOD1(block_writes, void(Context *));
  MOCK_METHOD0(unblock_writes, void());

  MOCK_CONST_METHOD0(writes_empty, bool());
};

} // namespace librbd

#endif // CEPH_TEST_LIBRBD_MOCK_AIO_IMAGE_REQUEST_WQ_H
