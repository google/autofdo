// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "chromiumos-wide-profiling/compat/string.h"
#include "chromiumos-wide-profiling/compat/test.h"
#include "chromiumos-wide-profiling/perf_protobuf_io.h"
#include "chromiumos-wide-profiling/perf_reader.h"
#include "chromiumos-wide-profiling/perf_recorder.h"
#include "chromiumos-wide-profiling/perf_serializer.h"
#include "chromiumos-wide-profiling/run_command.h"
#include "chromiumos-wide-profiling/test_utils.h"

namespace quipper {

// Runs "perf record" to see if the command is available on the current system.
bool IsPerfRecordAvailable() {
  return RunCommand(
      {"perf", "record", "-a", "-o", "-", "--", "sleep", "1"},
      NULL);
}

TEST(PerfRecorderTest, TestRecord) {
  // Read perf data using the PerfReader class.
  // Dump it to a protobuf.
  // Read the protobuf, and reconstruct the perf data.
  quipper::PerfDataProto perf_data_proto;
  PerfRecorder perf_recorder;
  EXPECT_TRUE(perf_recorder.RecordAndConvertToProtobuf(
      {"sudo", GetPerfPath(), "record"}, 1, &perf_data_proto));
  EXPECT_GT(perf_data_proto.build_ids_size(), 0);
}

TEST(PerfRecorderTest, DontAllowCommands) {
  quipper::PerfDataProto perf_data_proto;
  PerfRecorder perf_recorder;
  EXPECT_FALSE(perf_recorder.RecordAndConvertToProtobuf(
      {"sudo", GetPerfPath(), "record" "--", "sh", "-c", "echo 'malicious'"},
      1, &perf_data_proto));
}

}  // namespace quipper

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  if (!quipper::IsPerfRecordAvailable())
    return 0;
  return RUN_ALL_TESTS();
}
