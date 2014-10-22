// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "perf_protobuf_io.h"
#include "perf_reader.h"
#include "perf_recorder.h"
#include "perf_serializer.h"
#include "quipper_string.h"
#include "quipper_test.h"
#include "test_utils.h"

namespace quipper {

TEST(PerfRecorderTest, TestRecord) {
  // Read perf data using the PerfReader class.
  // Dump it to a protobuf.
  // Read the protobuf, and reconstruct the perf data.
  quipper::PerfDataProto perf_data_proto;
  string perf_command_line = "sudo " + GetPerfPath() + " record";
  PerfRecorder perf_recorder;
  EXPECT_TRUE(perf_recorder.RecordAndConvertToProtobuf(perf_command_line,
                                                       1,
                                                       &perf_data_proto));
  EXPECT_GT(perf_data_proto.build_ids_size(), 0);
}

}  // namespace quipper

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
