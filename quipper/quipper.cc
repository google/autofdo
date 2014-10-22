// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sstream>
#include <string>

#include "base/logging.h"

#include "perf_protobuf_io.h"
#include "perf_reader.h"
#include "perf_recorder.h"
#include "quipper_string.h"

namespace {
const char kDefaultOutputFile[] = "/dev/stdout";
}

bool ParseArguments(int argc, char* argv[], string* perf_command_line,
                    int* duration) {
  if (argc < 3) {
    LOG(ERROR) << "Invalid command line.";
    LOG(ERROR) << "Usage: " << argv[0] <<
               " <duration in seconds>" <<
               " <path to perf>" <<
               " <perf arguments>";
    return false;
  }

  stringstream ss;
  ss << argv[1];
  ss >> *duration;

  for (int i = 2; i < argc; i++) {
    *perf_command_line += " ";
    *perf_command_line += argv[i];
  }
  return true;
}

// Usage is:
// <exe> <duration in seconds> <perf command line>
int main(int argc, char* argv[]) {
  string perf_command_line;
  int perf_duration;

  if (!ParseArguments(argc, argv, &perf_command_line, &perf_duration))
    return 1;

  quipper::PerfRecorder perf_recorder;
  quipper::PerfDataProto perf_data_proto;
  if (!perf_recorder.RecordAndConvertToProtobuf(perf_command_line,
                                                perf_duration,
                                                &perf_data_proto))
    return 1;
  if (!WriteProtobufToFile(perf_data_proto, kDefaultOutputFile))
    return 1;
  return 0;
}
