// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_PERF_RECORDER_H_
#define CHROMIUMOS_WIDE_PROFILING_PERF_RECORDER_H_

#include <string>
#include <vector>

#include "base/macros.h"

#include "chromiumos-wide-profiling/compat/proto.h"
#include "chromiumos-wide-profiling/compat/string.h"
#include "chromiumos-wide-profiling/perf_reader.h"

namespace quipper {

class PerfRecorder {
 public:
  PerfRecorder() {}
  bool RecordAndConvertToProtobuf(const std::vector<string>& perf_args,
                                  const int time,
                                  quipper::PerfDataProto* perf_data);
 private:
  DISALLOW_COPY_AND_ASSIGN(PerfRecorder);
};

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_PERF_RECORDER_H_
