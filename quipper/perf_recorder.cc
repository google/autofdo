// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "perf_recorder.h"
#include "perf_serializer.h"
#include "quipper_string.h"
#include "utils.h"

namespace quipper {

string PerfRecorder::GetSleepCommand(const int time) {
  stringstream ss;
  ss << "sleep " << time;
  return ss.str();
}

bool PerfRecorder::RecordAndConvertToProtobuf(
    const string& perf_command,
    const int time,
    quipper::PerfDataProto* perf_data) {
  string full_perf_command = perf_command + " -o - -- " + GetSleepCommand(time);

  std::vector<char> raw_perf_data;
  RunCommandAndGetStdout(full_perf_command, &raw_perf_data);

  // Now convert it into a protobuf.
  PerfSerializer perf_serializer;
  PerfSerializer::Options options;
  // Make sure to remap address for security reasons.
  options.do_remap = true;
  // Discard unused perf events to reduce the protobuf size.
  options.discard_unused_events = true;

  perf_serializer.set_options(options);

  return (perf_serializer.ReadFromVector(raw_perf_data) &&
          perf_serializer.Serialize(perf_data));
}

}  // namespace quipper
