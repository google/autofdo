// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(asharif): Move this and other utilities to contrib/ dir.
#include "chromiumos-wide-profiling/conversion_utils.h"

#include <stdlib.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <google/protobuf/text_format.h>

#include "base/logging.h"

#include "chromiumos-wide-profiling/perf_protobuf_io.h"
#include "chromiumos-wide-profiling/perf_serializer.h"
#include "chromiumos-wide-profiling/quipper_string.h"
#include "chromiumos-wide-profiling/utils.h"

using google::protobuf::TextFormat;

namespace quipper {

namespace {

// Parse options from the format strings, set the options, and return the base
// format. Returns the empty string if options are not recognized.
string ParseFormatOptions(string format, PerfParser::Options* options) {
  auto dot = format.find('.');
  if (dot != string::npos) {
    string opt = format.substr(dot+1);
    format = format.substr(0, dot);
    if (opt == "remap") {
      options->do_remap = true;
    } else if (opt == "discard") {
      options->discard_unused_events = true;
    } else if (opt == "remap.discard") {
      options->do_remap = true;
      options->discard_unused_events = true;
    } else {
      LOG(ERROR) << "Unknown option: " << opt;
      return "";
    }
  }
  return format;
}

// ReadInput reads the input and stores it within |perf_serializer|.
bool ReadInput(const FormatAndFile& input, PerfSerializer* perf_serializer) {
  LOG(INFO) << "Reading input.";

  PerfParser::Options options;
  string format = ParseFormatOptions(input.format, &options);
  perf_serializer->set_options(options);

  if (format == kPerfFormat) {
    return perf_serializer->ReadFile(input.filename);
  }

  if (format == kProtoTextFormat) {
    PerfDataProto perf_data_proto;
    std::vector<char> data;
    if (!FileToBuffer(input.filename, &data)) return false;
    string text(data.begin(), data.end());
    if (!TextFormat::ParseFromString(text, &perf_data_proto)) return false;

    if (!perf_serializer->Deserialize(perf_data_proto)) return false;
    return true;
  }

  LOG(ERROR) << "Unimplemented read format: " << input.format;
  return false;
}

// WriteOutput reads from |perf_serializer| and writes the output to the file
// within |output|.
bool WriteOutput(const FormatAndFile& output, PerfSerializer* perf_serializer) {
  LOG(INFO) << "Writing output.";
  string output_string;
  if (output.format == kPerfFormat) {
    return perf_serializer->WriteFile(output.filename);
  }

  if (output.format == kProtoTextFormat) {
    PerfDataProto perf_data_proto;
    perf_serializer->Serialize(&perf_data_proto);
    // Reset the timestamp field since it causes reproducability issues when
    // testing.
    perf_data_proto.set_timestamp_sec(0);
    if (!TextFormat::PrintToString(perf_data_proto, &output_string))
      return false;
    std::vector<char> data(output_string.begin(), output_string.end());
    return BufferToFile(output.filename, data);
  }

  LOG(ERROR) << "Unimplemented write format: " << output.format;
  return false;
}

}  // namespace

// Format string for perf.data.
const char kPerfFormat[] = "perf";

// Format string for protobuf text format.
const char kProtoTextFormat[] = "text";

bool ConvertFile(const FormatAndFile& input, const FormatAndFile& output) {
  quipper::PerfSerializer perf_serializer;
  if (!ReadInput(input, &perf_serializer)) return false;
  if (!WriteOutput(output, &perf_serializer)) return false;
  return true;
}

}  // namespace quipper
