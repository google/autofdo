// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/test_perf_data.h"

#include <ostream>  // NOLINT

#include "base/logging.h"

#include "chromiumos-wide-profiling/kernel/perf_internals.h"
#include "chromiumos-wide-profiling/utils.h"

namespace quipper {
namespace testing {

ExamplePerfDataFileHeader::ExamplePerfDataFileHeader(
    const size_t attr_count, const unsigned long features) {  // NOLINT
  CHECK_EQ(112U, sizeof(perf_file_attr)) << "perf_file_attr has changed size!";
  const size_t attrs_size = attr_count * sizeof(perf_file_attr);
  header_ = {
    .magic = kPerfMagic,
    .size = 104,
    .attr_size = sizeof(perf_file_attr),
    .attrs = {.offset = 104, .size = attrs_size},
    .data = {.offset = 104 + attrs_size, .size = (1+14)*sizeof(u64)},
    .event_types = {0},
    .adds_features = {features, 0, 0, 0},
  };
}

void ExamplePerfDataFileHeader::WriteTo(std::ostream* out) const {
  out->write(reinterpret_cast<const char*>(&header_), sizeof(header_));
  CHECK_EQ(static_cast<u64>(out->tellp()), header_.size);
  CHECK_EQ(static_cast<u64>(out->tellp()), header_.attrs.offset);
}

void ExamplePipedPerfDataFileHeader::WriteTo(std::ostream* out) const {
  const perf_pipe_file_header header = {
    .magic = kPerfMagic,
    .size = 16,
  };
  out->write(reinterpret_cast<const char*>(&header), sizeof(header));
  CHECK_EQ(static_cast<u64>(out->tellp()), header.size);
}

void ExamplePerfEventAttrEvent_Hardware::WriteTo(std::ostream* out) const {
  // Due to the unnamed union fields (eg, sample_period), this structure can't
  // be initialized with designated initializers.
  perf_event_attr attr = {};
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof(perf_event_attr);
  attr.config = 0;
  attr.sample_period = 100001;
  attr.sample_type = sample_type_;
  attr.sample_id_all = sample_id_all_;

  const attr_event event = {
    .header = {
      .type = PERF_RECORD_HEADER_ATTR,
      .misc = 0,
      .size = sizeof(attr_event),  // No ids to add to size.
    },
    .attr = attr,
  };

  out->write(reinterpret_cast<const char*>(&event), sizeof(event));
}

void ExamplePerfFileAttr_Tracepoint::WriteTo(std::ostream* out) const {
  // Due to the unnamed union fields (eg, sample_period), this structure can't
  // be initialized with designated initializers.
  perf_event_attr attr = {};
  // See kernel src: tools/perf/util/evsel.c perf_evsel__newtp()
  attr.type = PERF_TYPE_TRACEPOINT;
  attr.size = sizeof(perf_event_attr);
  attr.config = tracepoint_event_id_;
  attr.sample_period = 1;
  attr.sample_type = (PERF_SAMPLE_IP |
                      PERF_SAMPLE_TID |
                      PERF_SAMPLE_TIME |
                      PERF_SAMPLE_CPU |
                      PERF_SAMPLE_PERIOD |
                      PERF_SAMPLE_RAW);

  const perf_file_attr file_attr = {
    .attr = attr,
    .ids = {.offset = 104, .size = 0},
  };
  out->write(reinterpret_cast<const char*>(&file_attr), sizeof(file_attr));
}

void ExampleMmapEvent_Tid::WriteTo(std::ostream* out) const {
  const size_t filename_aligned_length =
      GetUint64AlignedStringLength(filename_);
  const size_t event_size =
      offsetof(struct mmap_event, filename) +
      filename_aligned_length +
      1*sizeof(u64);  // sample_id_all

  struct mmap_event event = {
    .header = {
      .type = PERF_RECORD_MMAP,
      .misc = 0,
      .size = static_cast<u16>(event_size),
    },
    .pid = pid_, .tid = pid_,
    .start = start_,
    .len = len_,
    .pgoff = pgoff_,
    // .filename = ..., // written separately
  };
  const u64 sample_id[] = {
    PunU32U64{.v32={pid_, pid_}}.v64,  // TID (u32 pid, tid)
  };

  const size_t pre_mmap_offset = out->tellp();
  out->write(reinterpret_cast<const char*>(&event),
             offsetof(struct mmap_event, filename));
  *out << filename_
       << string(filename_aligned_length - filename_.size(), '\0');
  out->write(reinterpret_cast<const char*>(sample_id), sizeof(sample_id));
  const size_t written_event_size =
      static_cast<size_t>(out->tellp()) - pre_mmap_offset;
  CHECK_EQ(event.header.size,
           static_cast<u64>(written_event_size));
}

void ExampleMmap2Event_Tid::WriteTo(std::ostream* out) const {
  const size_t filename_aligned_length =
      GetUint64AlignedStringLength(filename_);
  const size_t event_size =
      offsetof(struct mmap2_event, filename) +
      filename_aligned_length +
      1*sizeof(u64);  // sample_id_all

  struct mmap2_event event = {
    .header = {
      .type = PERF_RECORD_MMAP2,
      .misc = 0,
      .size = static_cast<u16>(event_size),
    },
    .pid = pid_, .tid = pid_,
    .start = start_,
    .len = len_,
    .pgoff = pgoff_,
    .maj = 6,
    .min = 7,
    .ino = 8,
    .ino_generation = 9,
    .prot = 1|2,  // == PROT_READ | PROT_WRITE
    .flags = 2,   // == MAP_PRIVATE
    // .filename = ..., // written separately
  };
  const u64 sample_id[] = {
    PunU32U64{.v32={pid_, pid_}}.v64,  // TID (u32 pid, tid)
  };

  const size_t pre_mmap_offset = out->tellp();
  out->write(reinterpret_cast<const char*>(&event),
             offsetof(struct mmap2_event, filename));
  *out << filename_
       << string(filename_aligned_length - filename_.size(), '\0');
  out->write(reinterpret_cast<const char*>(sample_id), sizeof(sample_id));
  const size_t written_event_size =
      static_cast<size_t>(out->tellp()) - pre_mmap_offset;
  CHECK_EQ(event.header.size,
           static_cast<u64>(written_event_size));
}

void ExamplePerfSampleEvent_IpTid::WriteTo(std::ostream* out) const {
  const sample_event event = {
    .header = {
      .type = PERF_RECORD_SAMPLE,
      .misc = PERF_RECORD_MISC_USER,
      .size = sizeof(struct sample_event) + 2*sizeof(u64),
    }
  };
  const u64 sample_event_array[] = {
    ip_,                               // IP
    PunU32U64{.v32={pid_, tid_}}.v64,  // TID (u32 pid, tid)
  };
  CHECK_EQ(event.header.size,
           sizeof(event.header) + sizeof(sample_event_array));
  out->write(reinterpret_cast<const char*>(&event), sizeof(event));
  out->write(reinterpret_cast<const char*>(sample_event_array),
             sizeof(sample_event_array));
}

void ExamplePerfSampleEvent_Tracepoint::WriteTo(std::ostream* out) const {
  const sample_event event = {
    .header = {
      .type = PERF_RECORD_SAMPLE,
      .misc = PERF_RECORD_MISC_USER,
      .size = 0x0078,
    }
  };
  const u64 sample_event_array[] = {
    0x00007f999c38d15a,  // IP
    0x0000068d0000068d,  // TID (u32 pid, tid)
    0x0001e0211cbab7b9,  // TIME
    0x0000000000000000,  // CPU
    0x0000000000000001,  // PERIOD
    0x0000004900000044,  // RAW (u32 size = 0x44 = 68 = 4 + 8*sizeof(u64))
    0x000000090000068d,  //  .
    0x0000000000000000,  //  .
    0x0000100000000000,  //  .
    0x0000000300000000,  //  .
    0x0000002200000000,  //  .
    0xffffffff00000000,  //  .
    0x0000000000000000,  //  .
    0x0000000000000000,  //  .
  };
  CHECK_EQ(event.header.size,
           sizeof(event.header) + sizeof(sample_event_array));
  out->write(reinterpret_cast<const char*>(&event), sizeof(event));
  out->write(reinterpret_cast<const char*>(sample_event_array),
             sizeof(sample_event_array));
}

static const char kTraceMetadataValue[] =
    "\x17\x08\x44tracing0.5BLAHBLAHBLAH....";

const std::vector<char> ExampleTracingMetadata::Data::kTraceMetadata(
    kTraceMetadataValue, kTraceMetadataValue+sizeof(kTraceMetadataValue)-1);

void ExampleTracingMetadata::Data::WriteTo(std::ostream* out) const {
  const perf_file_section &index_entry = parent_->index_entry_.index_entry_;
  CHECK_EQ(static_cast<u64>(out->tellp()), index_entry.offset);
  out->write(kTraceMetadata.data(), kTraceMetadata.size());
  CHECK_EQ(static_cast<u64>(out->tellp()),
           index_entry.offset + index_entry.size);
}

}  // namespace testing
}  // namespace quipper
