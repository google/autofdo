// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_PERF_PROTOBUF_IO_H_
#define CHROMIUMOS_WIDE_PROFILING_PERF_PROTOBUF_IO_H_

#include <string>
#include "quipper_proto.h"
#include "quipper_string.h"

namespace quipper {

bool WriteProtobufToFile(const quipper::PerfDataProto& perf_data_proto,
                         const string& filename);

bool ReadProtobufFromFile(quipper::PerfDataProto* perf_data_proto,
                          const string& filename);

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_PERF_PROTOBUF_IO_H_
