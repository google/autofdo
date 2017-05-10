// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/perf_parser.h"


#include <algorithm>
#include <cstdio>
#include <limits>
#include <set>

#include "base/logging.h"

#include "chromiumos-wide-profiling/address_mapper.h"
#include "chromiumos-wide-profiling/compat/string.h"
#include "chromiumos-wide-profiling/limits.h"
#include "chromiumos-wide-profiling/utils.h"

namespace quipper {

namespace {

struct EventAndTime {
  ParsedEvent* event;
  uint64_t time;
};

// Returns true if |e1| has an earlier timestamp than |e2|.  The args are const
// pointers instead of references because of the way this function is used when
// calling std::stable_sort.
bool CompareParsedEventTimes(const std::unique_ptr<EventAndTime>& e1,
                             const std::unique_ptr<EventAndTime>& e2) {
  return (e1->time < e2->time);
}

// Name and ID of the kernel swapper process.
const char kSwapperCommandName[] = "swapper";
const uint32_t kSwapperPid = 0;

bool IsNullBranchStackEntry(const struct branch_entry& entry) {
  return (!entry.from && !entry.to);
}

}  // namespace

PerfParser::PerfParser() {}

PerfParser::~PerfParser() {}

PerfParser::PerfParser(const PerfParser::Options& options) {
  options_ = options;
}

void PerfParser::set_options(const PerfParser::Options& options) {
  options_ = options;
}

bool PerfParser::ParseRawEvents() {
  process_mappers_.clear();
  parsed_events_.resize(reader_.events().size());
  for (size_t i = 0; i < reader_.events().size(); ++i) {
    ParsedEvent& parsed_event = parsed_events_[i];
    parsed_event.raw_event = reader_.events()[i].get();
  }
  ProcessEvents();

  if (!options_.discard_unused_events)
    return true;

  // Some MMAP/MMAP2 events' mapped regions will not have any samples. These
  // MMAP/MMAP2 events should be dropped. |parsed_events_| should be
  // reconstructed without these events.
  size_t write_index = 0;
  size_t read_index;
  for (read_index = 0; read_index < parsed_events_.size(); ++read_index) {
    const ParsedEvent& event = parsed_events_[read_index];
    if ((event.raw_event->header.type == PERF_RECORD_MMAP ||
         event.raw_event->header.type == PERF_RECORD_MMAP2) &&
        event.num_samples_in_mmap_region == 0) {
      continue;
    }
    if (read_index != write_index)
      parsed_events_[write_index] = event;
    ++write_index;
  }
  CHECK_LE(write_index, parsed_events_.size());
  parsed_events_.resize(write_index);

  // Now regenerate the sorted event list again.  These are pointers to events
  // so they must be regenerated after a resize() of the ParsedEvent vector.
  MaybeSortParsedEvents();

  return true;
}

bool PerfParser::ProcessEvents() {
  MaybeSortParsedEvents();
  memset(&stats_, 0, sizeof(stats_));

  stats_.did_remap = false;   // Explicitly clear the remap flag.

  // Pid 0 is called the swapper process. Even though perf does not record a
  // COMM event for pid 0, we act like we did receive a COMM event for it. Perf
  // does this itself, example:
  //   http://lxr.free-electrons.com/source/tools/perf/util/session.c#L1120
  commands_.insert(kSwapperCommandName);
  pidtid_to_comm_map_[std::make_pair(kSwapperPid, kSwapperPid)] =
      &(*commands_.find(kSwapperCommandName));

  std::map<string, string> filenames_to_build_ids;
  reader_.GetFilenamesToBuildIDs(&filenames_to_build_ids);
  const auto& build_id_for_filename =
      [&filenames_to_build_ids](const string& filename) -> string {
    const auto it = filenames_to_build_ids.find(filename);
    return (it != filenames_to_build_ids.end()) ? it->second : "";
  };

  // NB: Not necessarily actually sorted by time.
  for (unsigned int i = 0; i < parsed_events_sorted_by_time_.size(); ++i) {
    ParsedEvent& parsed_event = *parsed_events_sorted_by_time_[i];
    event_t& event = *parsed_event.raw_event;
    switch (event.header.type) {
      case PERF_RECORD_SAMPLE:
        // SAMPLE doesn't have any fields to log at a fixed,
        // previously-endian-swapped location. This used to log ip.
        VLOG(1) << "SAMPLE";
        ++stats_.num_sample_events;

        if (MapSampleEvent(&parsed_event)) {
          ++stats_.num_sample_events_mapped;
        }
        break;
      case PERF_RECORD_MMAP: {
        VLOG(1) << "MMAP: " << event.mmap.filename;
        ++stats_.num_mmap_events;
        // Use the array index of the current mmap event as a unique identifier.
        CHECK(MapMmapEvent(&event.mmap, i)) << "Unable to map MMAP event!";
        // No samples in this MMAP region yet, hopefully.
        parsed_event.num_samples_in_mmap_region = 0;
        DSOInfo dso_info;
        dso_info.name = event.mmap.filename;
        dso_info.build_id = build_id_for_filename(event.mmap.filename);
        dso_set_.insert(dso_info);
        break;
      }
      case PERF_RECORD_MMAP2: {
        VLOG(1) << "MMAP2: " << event.mmap2.filename;
        ++stats_.num_mmap_events;
        // Use the array index of the current mmap event as a unique identifier.
        CHECK(MapMmapEvent(&event.mmap2, i)) << "Unable to map MMAP2 event!";
        // No samples in this MMAP region yet, hopefully.
        parsed_event.num_samples_in_mmap_region = 0;
        DSOInfo dso_info;
        dso_info.name = event.mmap2.filename;
        dso_info.build_id = build_id_for_filename(event.mmap.filename);
        dso_set_.insert(dso_info);
        break;
      }
      case PERF_RECORD_FORK:
        VLOG(1) << "FORK: " << event.fork.ppid << ":" << event.fork.ptid
                << " -> " << event.fork.pid << ":" << event.fork.tid;
        ++stats_.num_fork_events;
        CHECK(MapForkEvent(event.fork)) << "Unable to map FORK event!";
        break;
      case PERF_RECORD_EXIT:
        // EXIT events have the same structure as FORK events.
        VLOG(1) << "EXIT: " << event.fork.ppid << ":" << event.fork.ptid;
        ++stats_.num_exit_events;
        break;
      case PERF_RECORD_COMM:
        VLOG(1) << "COMM: " << event.comm.pid << ":" << event.comm.tid << ": "
                << event.comm.comm;
        ++stats_.num_comm_events;
        CHECK(MapCommEvent(event.comm));
        commands_.insert(event.comm.comm);
        pidtid_to_comm_map_[std::make_pair(event.comm.pid, event.comm.tid)] =
            &(*commands_.find(event.comm.comm));
        break;
      case PERF_RECORD_LOST:
      case PERF_RECORD_THROTTLE:
      case PERF_RECORD_UNTHROTTLE:
      case PERF_RECORD_READ:
      case PERF_RECORD_USER_TYPE_START ... PERF_RECORD_HEADER_MAX:
        VLOG(1) << "Parsed event type: " << event.header.type
                << ". Doing nothing.";
        break;
      default:
        LOG(ERROR) << "Unknown event type: " << event.header.type;
        return false;
    }
  }
  // Print stats collected from parsing.
  LOG(INFO) << "Parser processed: "
            << stats_.num_mmap_events << " MMAP/MMAP2 events, "
            << stats_.num_comm_events << " COMM events, "
            << stats_.num_fork_events << " FORK events, "
            << stats_.num_exit_events << " EXIT events, "
            << stats_.num_sample_events << " SAMPLE events, "
            << stats_.num_sample_events_mapped << " of these were mapped";

  float sample_mapping_percentage =
      static_cast<float>(stats_.num_sample_events_mapped) /
      stats_.num_sample_events * 100.;
  float threshold = options_.sample_mapping_percentage_threshold;
  if (sample_mapping_percentage < threshold) {
    LOG(ERROR) << "Mapped " << static_cast<int>(sample_mapping_percentage)
               << "% of samples, expected at least "
               << static_cast<int>(threshold) << "%";
    return false;
  }
  stats_.did_remap = options_.do_remap;
  return true;
}

void PerfParser::MaybeSortParsedEvents() {
  if (!(reader_.sample_type() & PERF_SAMPLE_TIME)) {
    parsed_events_sorted_by_time_.resize(parsed_events_.size());
    for (size_t i = 0; i < parsed_events_.size(); ++i) {
      parsed_events_sorted_by_time_[i] = &parsed_events_[i];
    }
    return;
  }
  std::vector<std::unique_ptr<EventAndTime>> events_and_times;
  for (ParsedEvent& parsed_event : parsed_events_) {
    if (parsed_event.raw_event->header.type == PERF_RECORD_FINISHED_ROUND) {
      // This event doesn't have a timestamp, and is not needed.
      // TODO(dhsharp): Follow the pattern of perf's util/ordered_events to
      // use the partial-sorting of events between rounds to sort faster.
      continue;
    }

    std::unique_ptr<EventAndTime> event_and_time(new EventAndTime);

    // Store the timestamp and event pointer in an array.
    event_and_time->event = &parsed_event;

    struct perf_sample sample_info;
    CHECK(reader_.ReadPerfSampleInfo(*parsed_event.raw_event, &sample_info));
    event_and_time->time = sample_info.time;

    events_and_times.emplace_back(std::move(event_and_time));
  }
  // Sort the events based on timestamp, and then populate the sorted event
  // vector in sorted order.
  std::stable_sort(events_and_times.begin(), events_and_times.end(),
                   CompareParsedEventTimes);

  parsed_events_sorted_by_time_.resize(events_and_times.size());
  for (unsigned int i = 0; i < events_and_times.size(); ++i) {
    parsed_events_sorted_by_time_[i] = events_and_times[i]->event;
  }
}

bool PerfParser::MapSampleEvent(ParsedEvent* parsed_event) {
  bool mapping_failed = false;

  // Find the associated command.
  if (!(reader_.sample_type() & PERF_SAMPLE_IP &&
        reader_.sample_type() & PERF_SAMPLE_TID))
    return false;
  perf_sample sample_info;
  if (!reader_.ReadPerfSampleInfo(*parsed_event->raw_event, &sample_info))
    return false;
  PidTid pidtid = std::make_pair(sample_info.pid, sample_info.tid);
  const auto comm_iter = pidtid_to_comm_map_.find(pidtid);
  if (comm_iter != pidtid_to_comm_map_.end()) {
    parsed_event->set_command(comm_iter->second);
  }

  const uint64_t unmapped_event_ip = sample_info.ip;

  // Map the event IP itself.
  if (!MapIPAndPidAndGetNameAndOffset(sample_info.ip,
                                      sample_info.pid,
                                      &sample_info.ip,
                                      &parsed_event->dso_and_offset)) {
    mapping_failed = true;
  }

  if (sample_info.callchain &&
      !MapCallchain(sample_info.ip,
                    sample_info.pid,
                    unmapped_event_ip,
                    sample_info.callchain,
                    parsed_event)) {
    mapping_failed = true;
  }

  if (sample_info.branch_stack &&
      !MapBranchStack(sample_info.pid,
                      sample_info.branch_stack,
                      parsed_event)) {
    mapping_failed = true;
  }

  // Write the remapped data back to the raw event regardless of whether it was
  // entirely successfully remapped.  A single failed remap should not
  // invalidate all the other remapped entries.
  if (!reader_.WritePerfSampleInfo(sample_info, parsed_event->raw_event)) {
    LOG(ERROR) << "Failed to write back remapped sample info.";
    return false;
  }

  return !mapping_failed;
}

bool PerfParser::MapCallchain(const uint64_t ip,
                              const uint32_t pid,
                              const uint64_t original_event_addr,
                              struct ip_callchain* callchain,
                              ParsedEvent* parsed_event) {
  if (!callchain) {
    LOG(ERROR) << "NULL call stack data.";
    return false;
  }

  bool mapping_failed = false;

  // If the callchain's length is 0, there is no work to do.
  if (callchain->nr == 0)
    return true;

  // Keeps track of whether the current entry is kernel or user.
  parsed_event->callchain.resize(callchain->nr);
  int num_entries_mapped = 0;
  for (unsigned int j = 0; j < callchain->nr; ++j) {
    uint64_t entry = callchain->ips[j];
    // When a callchain context entry is found, do not attempt to symbolize it.
    if (entry >= PERF_CONTEXT_MAX) {
      continue;
    }
    // The sample address has already been mapped so no need to map it.
    if (entry == original_event_addr) {
      callchain->ips[j] = ip;
      continue;
    }
    if (!MapIPAndPidAndGetNameAndOffset(
            entry,
            pid,
            &callchain->ips[j],
            &parsed_event->callchain[num_entries_mapped++])) {
      mapping_failed = true;
    }
  }
  // Not all the entries were mapped.  Trim |parsed_event->callchain| to
  // remove unused entries at the end.
  parsed_event->callchain.resize(num_entries_mapped);

  return !mapping_failed;
}

bool PerfParser::MapBranchStack(const uint32_t pid,
                                struct branch_stack* branch_stack,
                                ParsedEvent* parsed_event) {
  if (!branch_stack) {
    LOG(ERROR) << "NULL branch stack data.";
    return false;
  }

  // First, trim the branch stack to remove trailing null entries.
  size_t trimmed_size = 0;
  for (size_t i = 0; i < branch_stack->nr; ++i) {
    // Count the number of non-null entries before the first null entry.
    if (IsNullBranchStackEntry(branch_stack->entries[i])) {
      break;
    }
    ++trimmed_size;
  }

  // If a null entry was found, make sure all subsequent null entries are NULL
  // as well.
  for (size_t i = trimmed_size; i < branch_stack->nr; ++i) {
    const struct branch_entry& entry = branch_stack->entries[i];
    if (!IsNullBranchStackEntry(entry)) {
      LOG(ERROR) << "Non-null branch stack entry found after null entry: "
                 << reinterpret_cast<void*>(entry.from) << " -> "
                 << reinterpret_cast<void*>(entry.to);
      return false;
    }
  }

  // Map branch stack addresses.
  parsed_event->branch_stack.resize(trimmed_size);
  for (unsigned int i = 0; i < trimmed_size; ++i) {
    struct branch_entry& entry = branch_stack->entries[i];
    ParsedEvent::BranchEntry& parsed_entry = parsed_event->branch_stack[i];
    if (!MapIPAndPidAndGetNameAndOffset(entry.from,
                                        pid,
                                        &entry.from,
                                        &parsed_entry.from)) {
      return false;
    }
    if (!MapIPAndPidAndGetNameAndOffset(entry.to,
                                        pid,
                                        &entry.to,
                                        &parsed_entry.to)) {
      return false;
    }
    parsed_entry.predicted = entry.flags.predicted;
    // Either predicted or mispredicted, not both. But don't use a CHECK here,
    // just exit gracefully because it's a minor issue.
    // We can't check this because Atom doesn't have predicted/mispredicted flags
  }

  return true;
}

bool PerfParser::MapIPAndPidAndGetNameAndOffset(
    uint64_t ip,
    uint32_t pid,
    uint64_t* new_ip,
    ParsedEvent::DSOAndOffset* dso_and_offset) {
  // Attempt to find the synthetic address of the IP sample in this order:
  // 1. Address space of the kernel.
  // 2. Address space of its own process.
  // 3. Address space of the parent process.

  uint64_t mapped_addr = 0;

  // Sometimes the first event we see is a SAMPLE event and we don't have the
  // time to create an address mapper for a process. Example, for pid 0.
  AddressMapper* mapper = GetOrCreateProcessMapper(pid).first;
  bool mapped = mapper->GetMappedAddress(ip, &mapped_addr);
  // TODO(asharif): What should we do when we cannot map a SAMPLE event?

  if (mapped) {
    if (dso_and_offset) {
      uint64_t id = kUint64Max;
      CHECK(mapper->GetMappedIDAndOffset(ip, &id, &dso_and_offset->offset_));
      // Make sure the ID points to a valid event.
      CHECK_LE(id, parsed_events_sorted_by_time_.size());
      ParsedEvent* parsed_event = parsed_events_sorted_by_time_[id];
      const event_t* raw_event = parsed_event->raw_event;

      DSOInfo dso_info;
      if (raw_event->header.type == PERF_RECORD_MMAP) {
        dso_info.name = raw_event->mmap.filename;
      } else if (raw_event->header.type == PERF_RECORD_MMAP2) {
        dso_info.name = raw_event->mmap2.filename;
      } else {
        LOG(FATAL) << "Expected MMAP or MMAP2 event";
      }

      // Find the mmap DSO filename in the set of known DSO names.
      // TODO(dhsharp): the comparator for DSOInfo only uses the dso name,
      // excluding buildid, so that this lookup works. Consider if std::set
      // is actually the right data structure for this task.
      std::set<DSOInfo>::const_iterator dso_iter = dso_set_.find(dso_info);
      CHECK(dso_iter != dso_set_.end());
      dso_and_offset->dso_info_ = &(*dso_iter);

      ++parsed_event->num_samples_in_mmap_region;
    }
    if (options_.do_remap)
      *new_ip = mapped_addr;
  }
  return mapped;
}

bool PerfParser::MapMmapEvent(uint64_t id,
                              uint32_t pid,
                              uint64_t* p_start,
                              uint64_t* p_len,
                              uint64_t* p_pgoff) {
  // We need to hide only the real kernel addresses.  However, to make things
  // more secure, and make the mapping idempotent, we should remap all
  // addresses, both kernel and non-kernel.

  AddressMapper* mapper = GetOrCreateProcessMapper(pid).first;

  uint64_t start = *p_start;
  uint64_t len = *p_len;
  uint64_t pgoff = *p_pgoff;

  // |id| == 0 corresponds to the kernel mmap. We have several cases here:
  //
  // For ARM and x86, in sudo mode, pgoff == start, example:
  // start=0x80008200
  // pgoff=0x80008200
  // len  =0xfffffff7ff7dff
  //
  // For x86-64, in sudo mode, pgoff is between start and start + len. SAMPLE
  // events lie between pgoff and pgoff + length of the real kernel binary,
  // example:
  // start=0x3bc00000
  // pgoff=0xffffffffbcc00198
  // len  =0xffffffff843fffff
  // SAMPLE events will be found after pgoff. For kernels with ASLR, pgoff will
  // be something only visible to the root user, and will be randomized at
  // startup. With |remap| set to true, we should hide pgoff in this case. So we
  // normalize all SAMPLE events relative to pgoff.
  //
  // For non-sudo mode, the kernel will be mapped from 0 to the pointer limit,
  // example:
  // start=0x0
  // pgoff=0x0
  // len  =0xffffffff
  if (id == 0) {
    // If pgoff is between start and len, we normalize the event by setting
    // start to be pgoff just like how it is for ARM and x86. We also set len to
    // be a much smaller number (closer to the real length of the kernel binary)
    // because SAMPLEs are actually only seen between |event->pgoff| and
    // |event->pgoff + kernel text size|.
    if (pgoff > start && pgoff < start + len) {
      len = len + start - pgoff;
      start = pgoff;
    }
    // For kernels with ALSR pgoff is critical information that should not be
    // revealed when |remap| is true.
    pgoff = 0;
  }

  if (!mapper->MapWithID(start, len, id, pgoff, true)) {
    mapper->DumpToLog();
    return false;
  }

  if (options_.do_remap) {
    uint64_t mapped_addr;
    CHECK(mapper->GetMappedAddress(start, &mapped_addr));
    *p_start = mapped_addr;
    *p_len = len;
    *p_pgoff = pgoff;
  }
  return true;
}

std::pair<AddressMapper*, bool> PerfParser::GetOrCreateProcessMapper(
    uint32_t pid, uint32_t ppid) {
  const auto& search = process_mappers_.find(pid);
  if (search != process_mappers_.end()) {
    return std::make_pair(search->second.get(), false);
  }

  std::unique_ptr<AddressMapper> mapper;
  const auto& parent_mapper = process_mappers_.find(ppid);
  if (parent_mapper != process_mappers_.end())
    mapper.reset(new AddressMapper(*parent_mapper->second));
  else
    mapper.reset(new AddressMapper());

  const auto inserted =
      process_mappers_.insert(search, std::make_pair(pid, std::move(mapper)));
  return std::make_pair(inserted->second.get(), true);
}

bool PerfParser::MapCommEvent(const struct comm_event& event) {
  GetOrCreateProcessMapper(event.pid);
  return true;
}

bool PerfParser::MapForkEvent(const struct fork_event& event) {
  PidTid parent = std::make_pair(event.ppid, event.ptid);
  PidTid child = std::make_pair(event.pid, event.tid);
  if (parent != child &&
      pidtid_to_comm_map_.find(parent) != pidtid_to_comm_map_.end()) {
    pidtid_to_comm_map_[child] = pidtid_to_comm_map_[parent];
  }

  const uint32_t pid = event.pid;

  // If the parent and child pids are the same, this is just a new thread
  // within the same process, so don't do anything.
  if (event.ppid == pid)
    return true;

  if (!GetOrCreateProcessMapper(pid, event.ppid).second) {
    DLOG(INFO) << "Found an existing process mapper with pid: " << pid;
  }

  return true;
}

}  // namespace quipper
