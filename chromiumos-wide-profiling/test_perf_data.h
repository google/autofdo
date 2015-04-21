// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_TEST_PERF_DATA_H_
#define CHROMIUMOS_WIDE_PROFILING_TEST_PERF_DATA_H_

#include <ostream>  // NOLINT
#include <vector>

#include "chromiumos-wide-profiling/kernel/perf_internals.h"
#include "chromiumos-wide-profiling/quipper_string.h"

namespace quipper {
namespace testing {

// Union for punning 32-bit words into a 64-bit word.
union PunU32U64 {
  u32 v32[2];
  u64 v64;
};

class StreamWriteable {
 public:
  virtual ~StreamWriteable() {}
  virtual void WriteTo(std::ostream* out) const = 0;
};

// Normal mode header
class ExamplePerfDataFileHeader : public StreamWriteable {
 public:
  explicit ExamplePerfDataFileHeader(const size_t attr_count,
                                     const unsigned long features);  // NOLINT

  const perf_file_header &header() const { return header_; }
  const ssize_t data_end() const {
    return static_cast<ssize_t>(header_.data.offset + header_.data.size); }

  void WriteTo(std::ostream* out) const override;

 private:
  perf_file_header header_;
};

// Produces the pipe-mode file header.
class ExamplePipedPerfDataFileHeader : public StreamWriteable {
 public:
  ExamplePipedPerfDataFileHeader() {}
  void WriteTo(std::ostream* out) const override;
};

// Produces a PERF_RECORD_HEADER_ATTR event with struct perf_event_attr
// describing a hardware event. The sample_type mask and the sample_id_all
// bit are paramatized.
class ExamplePerfEventAttrEvent_Hardware : public StreamWriteable {
 public:
  explicit ExamplePerfEventAttrEvent_Hardware(u64 sample_type,
                                              bool sample_id_all)
      : sample_type_(sample_type), sample_id_all_(sample_id_all) {}
  void WriteTo(std::ostream* out) const override;
 private:
  const u64 sample_type_;
  const bool sample_id_all_;
};

// Produces a struct perf_file_attr with a perf_event_attr describing a
// tracepoint event.
class ExamplePerfFileAttr_Tracepoint : public StreamWriteable {
 public:
  explicit ExamplePerfFileAttr_Tracepoint(const u64 tracepoint_event_id)
      : tracepoint_event_id_(tracepoint_event_id) {}
  void WriteTo(std::ostream* out) const override;
 private:
  const u64 tracepoint_event_id_;
};

// Produces a PERF_RECORD_MMAP event with the given file and mapping.
// Assumes attr.sample_id_all and PERF_SAMPLE_TID
class ExampleMmapEvent_Tid : public StreamWriteable {
 public:
  // pid is used as both pid and tid.
  ExampleMmapEvent_Tid(u32 pid, u64 start, u64 len, u64 pgoff, string filename)
      : pid_(pid),
        start_(start),
        len_(len),
        pgoff_(pgoff),
        filename_(filename) {
  }
  void WriteTo(std::ostream* out) const override;
 private:
  const u32 pid_;
  const u64 start_;
  const u64 len_;
  const u64 pgoff_;
  const string filename_;
};

// Produces a PERF_RECORD_MMAP2 event with the given file and mapping.
// Assumes attr.sample_id_all and PERF_SAMPLE_TID
class ExampleMmap2Event_Tid : public StreamWriteable {
 public:
  // pid is used as both pid and tid.
  ExampleMmap2Event_Tid(u32 pid, u64 start, u64 len, u64 pgoff, string filename)
      : pid_(pid),
        start_(start),
        len_(len),
        pgoff_(pgoff),
        filename_(filename) {
  }
  void WriteTo(std::ostream* out) const override;
 private:
  const u32 pid_;
  const u64 start_;
  const u64 len_;
  const u64 pgoff_;
  const string filename_;
};

// Produces a simple PERF_RECORD_SAMPLE event for a sample_type of
// PERF_SAMPLE_IP | PERF_SAMPLE_TID
class ExamplePerfSampleEvent_IpTid : public StreamWriteable {
 public:
  ExamplePerfSampleEvent_IpTid(u64 ip, u32 pid, u32 tid)
      : ip_(ip), pid_(pid), tid_(tid) {}
  void WriteTo(std::ostream* out) const override;
 private:
  const u64 ip_;
  const u32 pid_;
  const u32 tid_;
};

// Produces a struct sample_event matching ExamplePerfFileAttr_Tracepoint.
class ExamplePerfSampleEvent_Tracepoint : public StreamWriteable {
 public:
  ExamplePerfSampleEvent_Tracepoint() {}
  void WriteTo(std::ostream* out) const override;
};

// Produces a struct perf_file_section suitable for use in the metadata index.
class MetadataIndexEntry : public StreamWriteable {
 public:
  MetadataIndexEntry(u64 offset, u64 size)
    : index_entry_{.offset = offset, .size = size} {}
  void WriteTo(std::ostream* out) const override {
    out->write(reinterpret_cast<const char*>(&index_entry_),
               sizeof(index_entry_));
  }
 public:
  const perf_file_section index_entry_;
};

// Produces sample tracing metadata, and corresponding metadata index entry.
class ExampleTracingMetadata {
 public:
  class Data : public StreamWriteable {
   public:
    static const std::vector<char> kTraceMetadata;

    explicit Data(ExampleTracingMetadata* parent) : parent_(parent) {}

    const std::vector<char> value() const { return kTraceMetadata; }

    void WriteTo(std::ostream* out) const override;

   private:
    ExampleTracingMetadata* parent_;
  };

  explicit ExampleTracingMetadata(size_t offset)
      : data_(this), index_entry_(offset, data_.value().size()) {}

  const Data &data() { return data_; }
  const MetadataIndexEntry &index_entry() { return index_entry_; }

 private:
  friend class Data;
  Data data_;
  MetadataIndexEntry index_entry_;
};

}  // namespace testing
}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_TEST_PERF_DATA_H_
