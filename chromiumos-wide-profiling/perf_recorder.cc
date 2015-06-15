// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/perf_recorder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "chromiumos-wide-profiling/compat/string.h"
#include "chromiumos-wide-profiling/perf_serializer.h"
#include "chromiumos-wide-profiling/run_command.h"
#include "chromiumos-wide-profiling/scoped_temp_path.h"
#include "chromiumos-wide-profiling/utils.h"

namespace quipper {

namespace {
string IntToString(const int i) {
  stringstream ss;
  ss << i;
  return ss.str();
}
}  // namespace

bool PerfRecorder::RecordAndConvertToProtobuf(
    const std::vector<string>& perf_args,
    const int time,
    quipper::PerfDataProto* perf_data) {
  ScopedTempFile output_file;
  if (std::find(perf_args.begin(), perf_args.end(), "--") != perf_args.end()) {
    // Perf uses '--' to mark the end of arguments and the beginning of the
    // command to run and profile. We need to run sleep, so disallow this.
    LOG(ERROR) << "perf_args may contain a command. Refusing to run it!";
    return false;
  }
  std::vector<string> full_perf_args(perf_args.begin(), perf_args.end());
  full_perf_args.insert(full_perf_args.end(),
                        {"-o", output_file.path(),
                         "--",
                         "sleep", IntToString(time)});

  // The perf command writes the output to a file, so ignore stdout.
  if (!RunCommand(full_perf_args, nullptr)) {
    return false;
  }

  // Now convert it into a protobuf.
  PerfSerializer perf_serializer;
  PerfParser::Options options;
  // Make sure to remap address for security reasons.
  options.do_remap = true;
  // Discard unused perf events to reduce the protobuf size.
  options.discard_unused_events = true;

  perf_serializer.set_options(options);

  return perf_serializer.SerializeFromFile(output_file.path(), perf_data);
}

}  // namespace quipper
