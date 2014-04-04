// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "perf_serializer.h"

#include <bitset>

#include <stdio.h>
#include <sys/time.h>

#include "base/logging.h"

#include "quipper_string.h"
#include "utils.h"

namespace quipper {

PerfSerializer::PerfSerializer() {
}

PerfSerializer::~PerfSerializer() {
}

bool PerfSerializer::SerializeFromFile(const string& filename,
                                       PerfDataProto* perf_data_proto) {
  if (!ReadFile(filename))
    return false;
  return Serialize(perf_data_proto);
}

bool PerfSerializer::Serialize(PerfDataProto* perf_data_proto) {
  if (!SerializePerfFileAttrs(attrs_, perf_data_proto->mutable_file_attrs()) ||
      !SerializePerfEventTypes(event_types_,
                               perf_data_proto->mutable_event_types())) {
    return false;
  }

  if (!ParseRawEvents() ||
      !SerializeEvents(parsed_events_, perf_data_proto->mutable_events())) {
    return false;
  }

  perf_data_proto->add_metadata_mask(metadata_mask_);

  if (!SerializeBuildIDs(
          build_id_events_, perf_data_proto->mutable_build_ids()) ||
      !SerializeStringMetadata(
          string_metadata_, perf_data_proto->mutable_string_metadata()) ||
      !SerializeUint32Metadata(
          uint32_metadata_, perf_data_proto->mutable_uint32_metadata()) ||
      !SerializeUint64Metadata(
          uint64_metadata_, perf_data_proto->mutable_uint64_metadata()) ||
      !SerializeCPUTopologyMetadata(
          cpu_topology_, perf_data_proto->mutable_cpu_topology()) ||
      !SerializeNUMATopologyMetadata(
          numa_topology_, perf_data_proto->mutable_numa_topology())) {
    return false;
  }

  // Add a timestamp_sec to the protobuf.
  struct timeval timestamp_sec;
  if (!gettimeofday(&timestamp_sec, NULL))
    perf_data_proto->set_timestamp_sec(timestamp_sec.tv_sec);

  PerfDataProto_PerfEventStats* stats = perf_data_proto->mutable_stats();
  stats->set_num_sample_events(stats_.num_sample_events);
  stats->set_num_mmap_events(stats_.num_mmap_events);
  stats->set_num_fork_events(stats_.num_fork_events);
  stats->set_num_exit_events(stats_.num_exit_events);
  stats->set_did_remap(stats_.did_remap);
  stats->set_num_sample_events_mapped(stats_.num_sample_events_mapped);
  return true;
}

bool PerfSerializer::DeserializeToFile(const PerfDataProto& perf_data_proto,
                                       const string& filename) {
  return Deserialize(perf_data_proto) && WriteFile(filename);
}

bool PerfSerializer::Deserialize(const PerfDataProto& perf_data_proto) {
  if (!DeserializePerfFileAttrs(perf_data_proto.file_attrs(), &attrs_) ||
      !DeserializePerfEventTypes(perf_data_proto.event_types(),
                                 &event_types_)) {
    return false;
  }

  // Make sure all event types (attrs) have the same sample type.
  for (size_t i = 0; i < attrs_.size(); ++i) {
    CHECK_EQ(attrs_[i].attr.sample_type, attrs_[0].attr.sample_type)
        << "Sample type for attribute #" << i
        << " (" << std::hex << attrs_[i].attr.sample_type << ")"
        << " does not match that of attribute 0"
        << " (" << std::hex << attrs_[0].attr.sample_type << ")";
  }
  CHECK_GT(attrs_.size(), 0U);
  sample_type_ = attrs_[0].attr.sample_type;

  SetRawEvents(perf_data_proto.events().size());
  if (!DeserializeEvents(perf_data_proto.events(), &parsed_events_))
    return false;

  if (perf_data_proto.metadata_mask_size())
    metadata_mask_ = perf_data_proto.metadata_mask(0);

  if (!DeserializeBuildIDs(perf_data_proto.build_ids(),
                           &build_id_events_) ||
      !DeserializeStringMetadata(perf_data_proto.string_metadata(),
                                 &string_metadata_) ||
      !DeserializeUint32Metadata(perf_data_proto.uint32_metadata(),
                                 &uint32_metadata_) ||
      !DeserializeUint64Metadata(perf_data_proto.uint64_metadata(),
                                 &uint64_metadata_) ||
      !DeserializeCPUTopologyMetadata(perf_data_proto.cpu_topology(),
                                      &cpu_topology_) ||
      !DeserializeNUMATopologyMetadata(perf_data_proto.numa_topology(),
                                       &numa_topology_)) {
    return false;
  }

  SortParsedEvents();
  if (!ProcessEvents())
    return false;

  memset(&stats_, 0, sizeof(stats_));
  const PerfDataProto_PerfEventStats& stats = perf_data_proto.stats();
  stats_.num_sample_events = stats.num_sample_events();
  stats_.num_mmap_events = stats.num_mmap_events();
  stats_.num_fork_events = stats.num_fork_events();
  stats_.num_exit_events = stats.num_exit_events();
  stats_.did_remap = stats.did_remap();
  stats_.num_sample_events_mapped = stats.num_sample_events_mapped();

  return true;
}

bool PerfSerializer::SerializePerfFileAttr(
    const PerfFileAttr& perf_file_attr,
    PerfDataProto_PerfFileAttr* perf_file_attr_proto) const {
  if (!SerializePerfEventAttr(perf_file_attr.attr,
                              perf_file_attr_proto->mutable_attr())) {
    return false;
  }

  for (size_t i = 0; i < perf_file_attr.ids.size(); i++ )
    perf_file_attr_proto->add_ids(perf_file_attr.ids[i]);
  return true;
}

bool PerfSerializer::DeserializePerfFileAttr(
    const PerfDataProto_PerfFileAttr& perf_file_attr_proto,
    PerfFileAttr* perf_file_attr) const {
  if (!DeserializePerfEventAttr(perf_file_attr_proto.attr(),
                                &perf_file_attr->attr)) {
    return false;
  }

  for (int i = 0; i < perf_file_attr_proto.ids_size(); i++ )
    perf_file_attr->ids.push_back(perf_file_attr_proto.ids(i));
  return true;
}

bool PerfSerializer::SerializePerfEventAttr(
    const perf_event_attr& perf_event_attr,
    PerfDataProto_PerfEventAttr* perf_event_attr_proto) const {
#define S(x) perf_event_attr_proto->set_##x(perf_event_attr.x)
  S(type);
  S(size);
  S(config);
  if (perf_event_attr_proto->freq())
    S(sample_freq);
  else
    S(sample_period);
  S(sample_type);
  S(read_format);
  S(disabled);
  S(inherit);
  S(pinned);
  S(exclusive);
  S(exclude_user);
  S(exclude_kernel);
  S(exclude_hv);
  S(exclude_idle);
  S(mmap);
  S(comm);
  S(freq);
  S(inherit_stat);
  S(enable_on_exec);
  S(task);
  S(watermark);
  S(precise_ip);
  S(mmap_data);
  S(sample_id_all);
  S(exclude_host);
  S(exclude_guest);
  if (perf_event_attr_proto->watermark())
    S(wakeup_watermark);
  else
    S(wakeup_events);
  S(bp_type);
  S(bp_len);
  S(branch_sample_type);
#undef S
  return true;
}

bool PerfSerializer::DeserializePerfEventAttr(
    const PerfDataProto_PerfEventAttr& perf_event_attr_proto,
    perf_event_attr* perf_event_attr) const {
  memset(perf_event_attr, 0, sizeof(*perf_event_attr));
#define S(x) perf_event_attr->x = perf_event_attr_proto.x()
  S(type);
  S(size);
  S(config);
  if (perf_event_attr->freq)
    S(sample_freq);
  else
    S(sample_period);
  S(sample_type);
  S(read_format);
  S(disabled);
  S(inherit);
  S(pinned);
  S(exclusive);
  S(exclude_user);
  S(exclude_kernel);
  S(exclude_hv);
  S(exclude_idle);
  S(mmap);
  S(comm);
  S(freq);
  S(inherit_stat);
  S(enable_on_exec);
  S(task);
  S(watermark);
  S(precise_ip);
  S(mmap_data);
  S(sample_id_all);
  S(exclude_host);
  S(exclude_guest);
  if (perf_event_attr->watermark)
    S(wakeup_watermark);
  else
    S(wakeup_events);
  S(bp_type);
  S(bp_len);
  S(branch_sample_type);
#undef S
  return true;
}

bool PerfSerializer::SerializePerfEventType(
    const perf_trace_event_type& event_type,
    quipper::PerfDataProto_PerfEventType* event_type_proto) const {
  event_type_proto->set_id(event_type.event_id);
  event_type_proto->set_name(event_type.name);
  event_type_proto->set_name_md5_prefix(Md5Prefix(event_type.name));
  return true;
}

bool PerfSerializer::DeserializePerfEventType(
    const quipper::PerfDataProto_PerfEventType& event_type_proto,
    perf_trace_event_type* event_type) const {
  event_type->event_id = event_type_proto.id();
  snprintf(event_type->name, arraysize(event_type->name), "%s",
           event_type_proto.name().c_str());
  return true;
}

bool PerfSerializer::SerializeEvent(
    const ParsedEvent& event,
    PerfDataProto_PerfEvent* event_proto) const {
  const perf_event_header& header = (*event.raw_event)->header;
  const event_t& raw_event = **event.raw_event;
  if (!SerializeEventHeader(header, event_proto->mutable_header()))
    return false;
  switch (header.type) {
    case PERF_RECORD_SAMPLE:
      if (!SerializeRecordSample(raw_event,
                                 event_proto->mutable_sample_event())) {
        return false;
      }
      break;
    case PERF_RECORD_MMAP:
      if (!SerializeMMapSample(raw_event, event_proto->mutable_mmap_event()))
        return false;
      break;
    case PERF_RECORD_COMM:
      if (!SerializeCommSample(raw_event, event_proto->mutable_comm_event()))
        return false;
      break;
    case PERF_RECORD_EXIT:
    case PERF_RECORD_FORK:
      if (!SerializeForkSample(raw_event, event_proto->mutable_fork_event()))
        return false;
      break;
    case PERF_RECORD_LOST:
      if (!SerializeLostSample(raw_event, event_proto->mutable_lost_event()))
        return false;
      break;
    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
      if (!SerializeThrottleSample(raw_event,
                                   event_proto->mutable_throttle_event())) {
        return false;
      }
      break;
    case PERF_RECORD_READ:
      if (!SerializeReadSample(raw_event, event_proto->mutable_read_event()))
        return false;
      break;
    default:
      LOG(ERROR) << "Unknown raw_event type: " << header.type;
      break;
  }
  return true;
}

bool PerfSerializer::DeserializeEvent(
    const PerfDataProto_PerfEvent& event_proto,
    ParsedEvent* event) const {
  // Here, event->raw_event points to a location in |events_|
  // However, the location doesn't contain a pointer to an event yet.
  // Since we don't know how much memory to allocate, use a local event_t for
  // now.
  event_t temp_event;
  memset(&temp_event, 0, sizeof(temp_event));
  if (!DeserializeEventHeader(event_proto.header(), &temp_event.header))
    return false;
  bool event_deserialized = true;
  switch (event_proto.header().type()) {
    case PERF_RECORD_SAMPLE:
      if (!DeserializeRecordSample(event_proto.sample_event(), &temp_event))
        event_deserialized = false;
      break;
    case PERF_RECORD_MMAP:
      if (!DeserializeMMapSample(event_proto.mmap_event(), &temp_event))
        event_deserialized = false;
      break;
    case PERF_RECORD_COMM:
      if (!DeserializeCommSample(event_proto.comm_event(), &temp_event))
        event_deserialized = false;
      break;
    case PERF_RECORD_EXIT:
    case PERF_RECORD_FORK:
      if (!DeserializeForkSample(event_proto.fork_event(), &temp_event))
        event_deserialized = false;
      break;
    case PERF_RECORD_LOST:
      if (!DeserializeLostSample(event_proto.lost_event(), &temp_event))
        event_deserialized = false;
      break;
    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
      if (!DeserializeThrottleSample(event_proto.throttle_event(), &temp_event))
        event_deserialized = false;
      break;
    case PERF_RECORD_READ:
      if (!DeserializeReadSample(event_proto.read_event(), &temp_event))
        event_deserialized = false;
      break;
    case PERF_RECORD_MAX:
    default:
      event_deserialized = false;
      break;
  }
  if (!event_deserialized) {
    *event->raw_event = NULL;
    return false;
  }
  // Copy the event from the stack to the heap.
  *event->raw_event = CallocMemoryForEvent(temp_event.header.size);
  memcpy(*event->raw_event, &temp_event, temp_event.header.size);
  return true;
}

bool PerfSerializer::SerializeEventHeader(
    const perf_event_header& header,
    PerfDataProto_EventHeader* header_proto) const {
  header_proto->set_type(header.type);
  header_proto->set_misc(header.misc);
  header_proto->set_size(header.size);
  return true;
}

bool PerfSerializer::DeserializeEventHeader(
    const PerfDataProto_EventHeader& header_proto,
    perf_event_header* header) const {
  header->type = header_proto.type();
  header->misc = header_proto.misc();
  header->size = header_proto.size();
  return true;
}

bool PerfSerializer::SerializeRecordSample(
    const event_t& event,
    PerfDataProto_SampleEvent* sample) const {
  perf_sample sample_info;
  if (!ReadPerfSampleInfo(event, &sample_info))
    return false;
  const struct ip_event& ip_event = event.ip;

  if (sample_type_ & PERF_SAMPLE_IP)
    sample->set_ip(ip_event.ip);
  if (sample_type_ & PERF_SAMPLE_TID) {
    CHECK_EQ(ip_event.pid, sample_info.pid);
    CHECK_EQ(ip_event.tid, sample_info.tid);
    sample->set_pid(ip_event.pid);
    sample->set_tid(ip_event.tid);
  }
  if (sample_type_ & PERF_SAMPLE_TIME)
    sample->set_sample_time_ns(sample_info.time);
  if (sample_type_ & PERF_SAMPLE_ADDR)
    sample->set_addr(sample_info.addr);
  if (sample_type_ & PERF_SAMPLE_ID)
    sample->set_id(sample_info.id);
  if (sample_type_ & PERF_SAMPLE_STREAM_ID)
    sample->set_stream_id(sample_info.stream_id);
  if (sample_type_ & PERF_SAMPLE_CPU)
    sample->set_cpu(sample_info.cpu);
  if (sample_type_ & PERF_SAMPLE_PERIOD)
    sample->set_period(sample_info.period);
  // TODO(sque): We don't have a use case for the raw data so just store the
  // size.  The data is assumed to be all zeroes.  So far it has been such.
  if (sample_type_ & PERF_SAMPLE_RAW)
    sample->set_raw_size(sample_info.raw_size);
  if (sample_type_ & PERF_SAMPLE_CALLCHAIN) {
    for (size_t i = 0; i < sample_info.callchain->nr; ++i)
      sample->add_callchain(sample_info.callchain->ips[i]);
  }
  if (sample_type_ & PERF_SAMPLE_BRANCH_STACK) {
    for (size_t i = 0; i < sample_info.branch_stack->nr; ++i) {
      sample->add_branch_stack();
      const struct branch_entry& entry = sample_info.branch_stack->entries[i];
      sample->mutable_branch_stack(i)->set_from_ip(entry.from);
      sample->mutable_branch_stack(i)->set_to_ip(entry.to);
      sample->mutable_branch_stack(i)->set_mispredicted(entry.flags.mispred);
    }
  }
  return true;
}

bool PerfSerializer::DeserializeRecordSample(
    const PerfDataProto_SampleEvent& sample,
    event_t* event) const {
  perf_sample sample_info;
  struct ip_event& ip_event = event->ip;
  if (sample.has_ip())
    ip_event.ip = sample.ip();
  if (sample.has_pid()) {
    CHECK(sample.has_tid()) << "Cannot have PID without TID.";
    ip_event.pid = sample.pid();
    ip_event.tid = sample.tid();
    sample_info.pid = sample.pid();
    sample_info.tid = sample.tid();
  }
  if (sample.has_sample_time_ns())
    sample_info.time = sample.sample_time_ns();
  if (sample.has_addr())
    sample_info.addr = sample.addr();
  if (sample.has_id())
    sample_info.id = sample.id();
  if (sample.has_stream_id())
    sample_info.stream_id = sample.stream_id();
  if (sample.has_cpu())
    sample_info.cpu = sample.cpu();
  if (sample.has_period())
    sample_info.period = sample.period();
  if (sample.callchain_size() > 0) {
    uint64 callchain_size = sample.callchain_size();
    sample_info.callchain =
        reinterpret_cast<struct ip_callchain*>(new uint64[callchain_size + 1]);
    sample_info.callchain->nr = callchain_size;
    for (size_t i = 0; i < callchain_size; ++i)
      sample_info.callchain->ips[i] = sample.callchain(i);
  }
  if (sample.raw_size() > 0) {
    sample_info.raw_size = sample.raw_size();
    sample_info.raw_data = new uint8[sample.raw_size()];
    memset(sample_info.raw_data, 0, sample.raw_size());
  }
  if (sample.branch_stack_size() > 0) {
    uint64 branch_stack_size = sample.branch_stack_size();
    sample_info.branch_stack =
        reinterpret_cast<struct branch_stack*>(
            new uint8[sizeof(uint64) +
                      branch_stack_size * sizeof(struct branch_entry)]);
    sample_info.branch_stack->nr = branch_stack_size;
    for (size_t i = 0; i < branch_stack_size; ++i) {
      struct branch_entry& entry = sample_info.branch_stack->entries[i];
      memset(&entry, 0, sizeof(entry));
      entry.from = sample.branch_stack(i).from_ip();
      entry.to = sample.branch_stack(i).to_ip();
      entry.flags.mispred = sample.branch_stack(i).mispredicted();
      entry.flags.predicted = !entry.flags.mispred;
    }
  }
  return WritePerfSampleInfo(sample_info, event);
}

bool PerfSerializer::SerializeMMapSample(
    const event_t& event,
    PerfDataProto_MMapEvent* sample) const {
  const struct mmap_event& mmap = event.mmap;
  sample->set_pid(mmap.pid);
  sample->set_tid(mmap.tid);
  sample->set_start(mmap.start);
  sample->set_len(mmap.len);
  sample->set_pgoff(mmap.pgoff);
  sample->set_filename(mmap.filename);
  sample->set_filename_md5_prefix(Md5Prefix(mmap.filename));

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeMMapSample(
    const PerfDataProto_MMapEvent& sample,
    event_t* event) const {
  struct mmap_event& mmap = event->mmap;
  mmap.pid = sample.pid();
  mmap.tid = sample.tid();
  mmap.start = sample.start();
  mmap.len = sample.len();
  mmap.pgoff = sample.pgoff();
  snprintf(mmap.filename, PATH_MAX, "%s", sample.filename().c_str());

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeCommSample(
      const event_t& event,
      PerfDataProto_CommEvent* sample) const {
  const struct comm_event& comm = event.comm;
  sample->set_pid(comm.pid);
  sample->set_tid(comm.tid);
  sample->set_comm(comm.comm);
  sample->set_comm_md5_prefix(Md5Prefix(comm.comm));

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeCommSample(
    const PerfDataProto_CommEvent& sample,
    event_t* event) const {
  struct comm_event& comm = event->comm;
  comm.pid = sample.pid();
  comm.tid = sample.tid();
  snprintf(comm.comm, sizeof(comm.comm), "%s", sample.comm().c_str());

  // Sometimes the command string will be modified.  e.g. if the original comm
  // string is not recoverable from the Md5sum prefix, then use the latter as a
  // replacement comm string.  However, if the original was < 8 bytes (fit into
  // |sizeof(uint64)|), then the size is no longer correct.  This section checks
  // for the size difference and updates the size in the header.
  uint64 sample_fields =
      GetSampleFieldsForEventType(comm.header.type, sample_type_);
  std::bitset<sizeof(sample_fields) * CHAR_BIT> sample_type_bits(sample_fields);
  comm.header.size = GetPerfSampleDataOffset(*event) +
                     sample_type_bits.count() * sizeof(uint64);

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeForkSample(
    const event_t& event,
    PerfDataProto_ForkEvent* sample) const {
  const struct fork_event& fork = event.fork;
  sample->set_pid(fork.pid);
  sample->set_ppid(fork.ppid);
  sample->set_tid(fork.tid);
  sample->set_ptid(fork.ppid);
  sample->set_fork_time_ns(fork.time);

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeForkSample(
    const PerfDataProto_ForkEvent& sample,
    event_t* event) const {
  struct fork_event& fork = event->fork;
  fork.pid = sample.pid();
  fork.ppid = sample.ppid();
  fork.tid = sample.tid();
  fork.ptid = sample.ptid();
  fork.time = sample.fork_time_ns();

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeLostSample(
    const event_t& event,
    PerfDataProto_LostEvent* sample) const {
  const struct lost_event& lost = event.lost;
  sample->set_id(lost.id);
  sample->set_lost(lost.lost);

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeLostSample(
    const PerfDataProto_LostEvent& sample,
    event_t* event) const {
  struct lost_event& lost = event->lost;
  lost.id = sample.id();
  lost.lost = sample.lost();

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeThrottleSample(
    const event_t& event,
    PerfDataProto_ThrottleEvent* sample) const {
  const struct throttle_event& throttle = event.throttle;
  sample->set_time(throttle.time);
  sample->set_id(throttle.id);
  sample->set_stream_id(throttle.stream_id);

  return SerializeSampleInfo(event, sample->mutable_sample_info());
}

bool PerfSerializer::DeserializeThrottleSample(
    const PerfDataProto_ThrottleEvent& sample,
    event_t* event) const {
  struct throttle_event& throttle = event->throttle;
  throttle.time = sample.time();
  throttle.id = sample.id();
  throttle.stream_id = sample.stream_id();

  return DeserializeSampleInfo(sample.sample_info(), event);
}

bool PerfSerializer::SerializeReadSample(
    const event_t& event,
    PerfDataProto_ReadEvent* sample) const {
  const struct read_event& read = event.read;
  sample->set_pid(read.pid);
  sample->set_tid(read.tid);
  sample->set_value(read.value);
  sample->set_time_enabled(read.time_enabled);
  sample->set_time_running(read.time_running);
  sample->set_id(read.id);

  return true;
}

bool PerfSerializer::DeserializeReadSample(
    const PerfDataProto_ReadEvent& sample,
    event_t* event) const {
  struct read_event& read = event->read;
  read.pid = sample.pid();
  read.tid = sample.tid();
  read.value = sample.value();
  read.time_enabled = sample.time_enabled();
  read.time_running = sample.time_running();
  read.id = sample.id();

  return true;
}

bool PerfSerializer::SerializeSampleInfo(
    const event_t& event,
    PerfDataProto_SampleInfo* sample) const {
  perf_sample sample_info;
  if (!ReadPerfSampleInfo(event, &sample_info))
    return false;

  if (sample_type_ & PERF_SAMPLE_TID) {
    sample->set_pid(sample_info.pid);
    sample->set_tid(sample_info.tid);
  }
  if (sample_type_ & PERF_SAMPLE_TIME)
    sample->set_sample_time_ns(sample_info.time);
  if (sample_type_ & PERF_SAMPLE_ID)
    sample->set_id(sample_info.id);
  if (sample_type_ & PERF_SAMPLE_CPU)
    sample->set_cpu(sample_info.cpu);
  return true;
}

bool PerfSerializer::DeserializeSampleInfo(
    const PerfDataProto_SampleInfo& sample,
    event_t* event) const {
  perf_sample sample_info;
  size_t sample_info_size = 0;
  if (sample.has_tid()) {
    sample_info.pid = sample.pid();
    sample_info.tid = sample.tid();
    sample_info_size += sizeof(uint64);
  }
  if (sample.has_sample_time_ns()) {
    sample_info.time = sample.sample_time_ns();
    sample_info_size += sizeof(uint64);
  }
  if (sample.has_id()) {
    sample_info.id = sample.id();
    sample_info_size += sizeof(uint64);
  }
  if (sample.has_cpu()) {
    sample_info.cpu = sample.cpu();
    sample_info_size += sizeof(uint64);
  }

  // The event info may have changed (e.g. strings replaced with Md5sum), so
  // adjust the size accordingly.
  event->header.size = GetPerfSampleDataOffset(*event) + sample_info_size;

  return WritePerfSampleInfo(sample_info, event);
}

bool PerfSerializer::SerializeBuildIDs(
    const std::vector<build_id_event*>& from,
    RepeatedPtrField<PerfDataProto_PerfBuildID>* to) const {
  return SerializeBuildIDEvents(from, to);
}

bool PerfSerializer::DeserializeBuildIDs(
    const RepeatedPtrField<PerfDataProto_PerfBuildID>& from,
    std::vector<build_id_event*>* to) const {
  // Free any existing build id events.
  for (size_t i = 0; i < to->size(); ++i)
    free(to->at(i));
  to->clear();

  return DeserializeBuildIDEvents(from, to);
}

bool PerfSerializer::SerializeBuildIDEvent(
    build_id_event* const& from,
    PerfDataProto_PerfBuildID* to) const {
  to->set_misc(from->header.misc);
  to->set_pid(from->pid);
  to->set_build_id_hash(from->build_id, kBuildIDArraySize);
  to->set_filename(from->filename);
  to->set_filename_md5_prefix(Md5Prefix(from->filename));
  return true;
}

bool PerfSerializer::DeserializeBuildIDEvent(
    const PerfDataProto_PerfBuildID& from,
    build_id_event** to) const {
  const string& filename = from.filename();
  size_t size = sizeof(build_id_event) + GetUint64AlignedStringLength(filename);

  build_id_event* event = CallocMemoryForBuildID(size);
  *to = event;
  event->header.type = PERF_RECORD_HEADER_BUILD_ID;
  event->header.size = size;
  event->header.misc = from.misc();
  event->pid = from.pid();
  memcpy(event->build_id, from.build_id_hash().c_str(), kBuildIDArraySize);

  CHECK_GT(snprintf(event->filename, filename.size() + 1, "%s",
                    filename.c_str()),
           0);
  return true;
}

bool PerfSerializer::SerializeSingleStringMetadata(
    const PerfStringMetadata& metadata,
    PerfDataProto_PerfStringMetadata* proto_metadata) const {
  proto_metadata->set_type(metadata.type);
  for (size_t i = 0; i < metadata.data.size(); ++i)
    proto_metadata->add_data(metadata.data[i].str);
  return true;
}

bool PerfSerializer::DeserializeSingleStringMetadata(
    const PerfDataProto_PerfStringMetadata& proto_metadata,
    PerfStringMetadata* metadata) const {
  metadata->type = proto_metadata.type();
  for (int i = 0; i < proto_metadata.data_size(); ++i) {
    CStringWithLength single_string;
    single_string.str = proto_metadata.data(i);
    single_string.len = GetUint64AlignedStringLength(single_string.str);
    metadata->data.push_back(single_string);
  }
  return true;
}

bool PerfSerializer::SerializeSingleUint32Metadata(
    const PerfUint32Metadata& metadata,
    PerfDataProto_PerfUint32Metadata* proto_metadata) const {
  proto_metadata->set_type(metadata.type);
  for (size_t i = 0; i < metadata.data.size(); ++i)
    proto_metadata->add_data(metadata.data[i]);
  return true;
}

bool PerfSerializer::DeserializeSingleUint32Metadata(
    const PerfDataProto_PerfUint32Metadata& proto_metadata,
    PerfUint32Metadata* metadata) const {
  metadata->type = proto_metadata.type();
  for (int i = 0; i < proto_metadata.data_size(); ++i)
    metadata->data.push_back(proto_metadata.data(i));
  return true;
}

bool PerfSerializer::SerializeSingleUint64Metadata(
    const PerfUint64Metadata& metadata,
    PerfDataProto_PerfUint64Metadata* proto_metadata) const {
  proto_metadata->set_type(metadata.type);
  for (size_t i = 0; i < metadata.data.size(); ++i)
    proto_metadata->add_data(metadata.data[i]);
  return true;
}

bool PerfSerializer::DeserializeSingleUint64Metadata(
    const PerfDataProto_PerfUint64Metadata& proto_metadata,
    PerfUint64Metadata* metadata) const {
  metadata->type = proto_metadata.type();
  for (int i = 0; i < proto_metadata.data_size(); ++i)
    metadata->data.push_back(proto_metadata.data(i));
  return true;
}

bool PerfSerializer::SerializeCPUTopologyMetadata(
    const PerfCPUTopologyMetadata& metadata,
    PerfDataProto_PerfCPUTopologyMetadata* proto_metadata) const {
  for (size_t i = 0; i < metadata.core_siblings.size(); ++i) {
    const string& str = metadata.core_siblings[i].str;
    proto_metadata->add_core_siblings(str);
    proto_metadata->add_core_siblings_md5_prefix(Md5Prefix(str));
  }

  for (size_t i = 0; i < metadata.thread_siblings.size(); ++i) {
    const string& str = metadata.thread_siblings[i].str;
    proto_metadata->add_thread_siblings(str);
    proto_metadata->add_thread_siblings_md5_prefix(Md5Prefix(str));
  }
  return true;
}

bool PerfSerializer::DeserializeCPUTopologyMetadata(
    const PerfDataProto_PerfCPUTopologyMetadata& proto_metadata,
    PerfCPUTopologyMetadata* metadata) const {
  for (int i = 0; i < proto_metadata.core_siblings_size(); ++i) {
    CStringWithLength core;
    core.str = proto_metadata.core_siblings(i);
    core.len = GetUint64AlignedStringLength(core.str);
    metadata->core_siblings.push_back(core);
  }

  for (int i = 0; i < proto_metadata.thread_siblings_size(); ++i) {
    CStringWithLength thread;
    thread.str = proto_metadata.thread_siblings(i);
    thread.len = GetUint64AlignedStringLength(thread.str);
    metadata->thread_siblings.push_back(thread);
  }
  return true;
}

bool PerfSerializer::SerializeNodeTopologyMetadata(
    const PerfNodeTopologyMetadata& metadata,
    PerfDataProto_PerfNodeTopologyMetadata* proto_metadata) const {
  proto_metadata->set_id(metadata.id);
  proto_metadata->set_total_memory(metadata.total_memory);
  proto_metadata->set_free_memory(metadata.free_memory);
  proto_metadata->set_cpu_list(metadata.cpu_list.str);
  proto_metadata->set_cpu_list_md5_prefix(Md5Prefix(metadata.cpu_list.str));
  return true;
}

bool PerfSerializer::DeserializeNodeTopologyMetadata(
    const PerfDataProto_PerfNodeTopologyMetadata& proto_metadata,
    PerfNodeTopologyMetadata* metadata) const {
  metadata->id = proto_metadata.id();
  metadata->total_memory = proto_metadata.total_memory();
  metadata->free_memory = proto_metadata.free_memory();
  metadata->cpu_list.str = proto_metadata.cpu_list();
  metadata->cpu_list.len = GetUint64AlignedStringLength(metadata->cpu_list.str);
  return true;
}

void PerfSerializer::SetRawEvents(size_t num_events) {
  events_.resize(num_events);
  parsed_events_.resize(num_events);
  for (size_t i = 0; i < num_events; ++i)
    parsed_events_[i].raw_event = &events_[i];
}

}  // namespace quipper
