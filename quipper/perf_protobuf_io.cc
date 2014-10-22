// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "base/logging.h"

#include "perf_protobuf_io.h"
#include "quipper_string.h"
#include "utils.h"

namespace quipper {

bool WriteProtobufToFile(const PerfDataProto& perf_data_proto,
                         const string& filename) {
  string target;
  perf_data_proto.SerializeToString(&target);

  std::vector<char> buffer(target.begin(), target.end());
  return BufferToFile(filename, buffer);
}

bool ReadProtobufFromFile(PerfDataProto* perf_data_proto,
                          const string& filename) {
  std::vector<char> buffer;
  if (!FileToBuffer(filename, &buffer))
    return false;

  bool ret = perf_data_proto->ParseFromArray(buffer.data(), buffer.size());

  LOG(INFO) << "#events" << perf_data_proto->events_size();

  return ret;
}

}  // namespace quipper
