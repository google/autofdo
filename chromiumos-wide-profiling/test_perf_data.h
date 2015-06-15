// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_TEST_PERF_DATA_H_
#define CHROMIUMOS_WIDE_PROFILING_TEST_PERF_DATA_H_

#include <ostream>  // NOLINT
#include <vector>

#include "chromiumos-wide-profiling/compat/string.h"
#include "chromiumos-wide-profiling/kernel/perf_internals.h"

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
                                     const u64 data_size,
                                     const unsigned long features);  // NOLINT

  const perf_file_header &header() const { return header_; }
  u64 data_end_offset() const {
    return header_.data.offset + header_.data.size;
  }
  ssize_t data_end() const {
    return static_cast<ssize_t>(data_end_offset());
  }

  void WriteTo(std::ostream* out) const override;

 protected:
  perf_file_header header_;
};

// Normal mode header with custom event attr size.
class ExamplePerfDataFileHeader_CustomAttrSize
    : public ExamplePerfDataFileHeader {
 public:
  explicit ExamplePerfDataFileHeader_CustomAttrSize(
      const size_t event_attr_size,
      const u64 data_size);  // NOLINT
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
  typedef ExamplePerfEventAttrEvent_Hardware SelfT;
  explicit ExamplePerfEventAttrEvent_Hardware(u64 sample_type,
                                              bool sample_id_all)
      : attr_size_(sizeof(perf_event_attr)),
        sample_type_(sample_type),
        sample_id_all_(sample_id_all),
        config_(0) {
  }
  SelfT& WithConfig(u64 config) { config_ = config; return *this; }
  SelfT& WithAttrSize(u32 size) { attr_size_ = size; return *this; }
  void WriteTo(std::ostream* out) const override;
 private:
  u32 attr_size_;
  const u64 sample_type_;
  const bool sample_id_all_;
  u64 config_;
};

// Produces a struct perf_file_attr with a perf_event_attr describing a
// hardware event.
class ExamplePerfFileAttr_Hardware : public StreamWriteable {
 public:
  typedef ExamplePerfFileAttr_Hardware SelfT;
  explicit ExamplePerfFileAttr_Hardware(u64 sample_type, bool sample_id_all)
      : attr_size_(sizeof(perf_event_attr)),
        sample_type_(sample_type),
        sample_id_all_(sample_id_all),
        config_(0) {
  }
  SelfT& WithAttrSize(u32 size) { attr_size_ = size; return *this; }
  SelfT& WithConfig(u64 config) { config_ = config; return *this; }
  void WriteTo(std::ostream* out) const override;
 private:
  u32 attr_size_;
  const u64 sample_type_;
  const bool sample_id_all_;
  u64 config_;
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

// Produces a sample field array that can be used with either SAMPLE events
// or as the sample_id of another event.
// NB: This class simply places the fields in the order called. It does not
// enforce that they are in the correct order, or match the sample type.
// See enum perf_event_type in perf_event.h.
class SampleInfo {
 public:
  SampleInfo& Ip(u64 ip) { return AddField(ip); }
  SampleInfo& Tid(u32 pid, u32 tid) {
    return AddField(PunU32U64{.v32 = {pid, tid}}.v64);
  }
  SampleInfo& Tid(u32 pid) {
    return AddField(PunU32U64{.v32 = {pid, pid}}.v64);
  }
  SampleInfo& Time(u64 time) { return AddField(time); }
  SampleInfo& BranchStack_nr(u64 nr) { return AddField(nr); }
  SampleInfo& BranchStack_lbr(u64 from, u64 to, u64 flags) {
    AddField(from);
    AddField(to);
    AddField(flags);
    return *this;
  }

  const char* data() const {
    return reinterpret_cast<const char *>(fields_.data());
  }
  const size_t size() const {
    return fields_.size() * sizeof(decltype(fields_)::value_type);
  }

 private:
  SampleInfo& AddField(u64 value) {
    fields_.push_back(value);
    return *this;
  }

  std::vector<u64> fields_;
};

// Produces a PERF_RECORD_MMAP event with the given file and mapping.
class ExampleMmapEvent : public StreamWriteable {
 public:
  ExampleMmapEvent(u32 pid, u64 start, u64 len, u64 pgoff, string filename,
                   const SampleInfo& sample_id)
      : pid_(pid),
        start_(start),
        len_(len),
        pgoff_(pgoff),
        filename_(filename),
        sample_id_(sample_id) {
  }
  void WriteTo(std::ostream* out) const override;
 private:
  const u32 pid_;
  const u64 start_;
  const u64 len_;
  const u64 pgoff_;
  const string filename_;
  const SampleInfo sample_id_;
};

// Produces a PERF_RECORD_MMAP2 event with the given file and mapping.
class ExampleMmap2Event : public StreamWriteable {
 public:
  // pid is used as both pid and tid.
  ExampleMmap2Event(u32 pid, u64 start, u64 len, u64 pgoff, string filename,
                    const SampleInfo& sample_id)
      : pid_(pid),
        start_(start),
        len_(len),
        pgoff_(pgoff),
        filename_(filename),
        sample_id_(sample_id) {
  }
  void WriteTo(std::ostream* out) const override;
 private:
  const u32 pid_;
  const u64 start_;
  const u64 len_;
  const u64 pgoff_;
  const string filename_;
  const SampleInfo sample_id_;
};

// Produces the PERF_RECORD_FINISHED_ROUND event. This event is just a header.
class FinishedRoundEvent : public StreamWriteable {
 public:
  void WriteTo(std::ostream* out) const override;
};

// Produces a simple PERF_RECORD_SAMPLE event with the given sample info.
// NB: The sample_info must match the sample_type of the relevant attr.
class ExamplePerfSampleEvent : public StreamWriteable {
 public:
  explicit ExamplePerfSampleEvent(const SampleInfo& sample_info)
      : sample_info_(sample_info) {
  }
  size_t GetSize() const;
  void WriteTo(std::ostream* out) const override;
 private:
  const SampleInfo sample_info_;
};

class ExamplePerfSampleEvent_BranchStack : public ExamplePerfSampleEvent {
 public:
  ExamplePerfSampleEvent_BranchStack();
  static const size_t kEventSize;
};

// Produces a struct sample_event matching ExamplePerfFileAttr_Tracepoint.
class ExamplePerfSampleEvent_Tracepoint : public StreamWriteable {
 public:
  ExamplePerfSampleEvent_Tracepoint() {}
  void WriteTo(std::ostream* out) const override;
  static const size_t kEventSize;
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

// Produces sample string metadata, and corresponding metadata index entry.
class ExampleStringMetadata : public StreamWriteable {
 public:
  // The input string gets zero-padded/truncated to |kStringAlignSize| bytes if
  // it is shorter/longer, respectively.
  explicit ExampleStringMetadata(const string& data, size_t offset)
      : data_(data),
        index_entry_(offset, sizeof(u32) + kStringAlignSize) {
    data_.resize(kStringAlignSize);
  }
  void WriteTo(std::ostream* out) const override;

  const MetadataIndexEntry& index_entry() { return index_entry_; }
  size_t size() const {
    return sizeof(u32) + data_.size();
  }

 private:
  string data_;
  MetadataIndexEntry index_entry_;

  static const int kStringAlignSize = 64;
};

// Produces sample string metadata event for piped mode.
class ExampleStringMetadataEvent : public StreamWriteable {
 public:
  // The input string gets aligned to |kStringAlignSize|.
  explicit ExampleStringMetadataEvent(u32 type, const string& data)
      : type_(type), data_(data) {
    data_.resize(kStringAlignSize);
  }
  void WriteTo(std::ostream* out) const override;

 private:
  u32 type_;
  string data_;

  static const int kStringAlignSize = 64;
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
