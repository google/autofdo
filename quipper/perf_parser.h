// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_PERF_PARSER_H_
#define CHROMIUMOS_WIDE_PROFILING_PERF_PARSER_H_

#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/macros.h"

#include "perf_reader.h"
#include "utils.h"

namespace quipper {

class AddressMapper;

// A struct containing all relevant info for a mapped DSO, independent of any
// samples.
struct DSOInfo {
  string name;
  string build_id;

  // Comparator that allows this to be stored in a STL set.
  bool operator<(const DSOInfo& other) const {
    if (name == other.name)
      return build_id < other.build_id;
    return name < other.name;
  }
};

struct ParsedEvent {
  // TODO(sque): Turn this struct into a class to privatize member variables.
  ParsedEvent() : command_(NULL) {}

  // Stores address of an event_t owned by the |PerfReader::events_| vector.
  event_t* raw_event;

  // For mmap events, use this to count the number of samples that are in this
  // region.
  uint32_t num_samples_in_mmap_region;

  // Command associated with this sample.
  const string* command_;

  // Accessor for command string.
  const string command() const {
    if (command_)
      return *command_;
    return string();
  }

  void set_command(const string& command) {
    command_ = &command;
  }

  // A struct that contains a DSO + offset pair.
  struct DSOAndOffset {
    const DSOInfo* dso_info_;
    uint64_t offset_;

    // Accessor methods.
    const string dso_name() const {
      if (dso_info_)
        return dso_info_->name;
      return string();
    }
    const string build_id() const {
      if (dso_info_)
        return dso_info_->build_id;
      return string();
    }
    uint64_t offset() const {
      return offset_;
    }

    DSOAndOffset() : dso_info_(NULL),
                     offset_(0) {}
  } dso_and_offset;

  // DSO+offset info for callchain.
  std::vector<DSOAndOffset> callchain;

  // DSO + offset info for branch stack entries.
  struct BranchEntry {
    bool predicted;
    DSOAndOffset from;
    DSOAndOffset to;
  };
  std::vector<BranchEntry> branch_stack;
};

struct PerfEventStats {
  // Number of each type of event.
  uint32_t num_sample_events;
  uint32_t num_mmap_events;
  uint32_t num_comm_events;
  uint32_t num_fork_events;
  uint32_t num_exit_events;

  // Number of sample events that were successfully mapped using the address
  // mapper.  The mapping is recorded regardless of whether the address in the
  // perf sample event itself was assigned the remapped address.  The latter is
  // indicated by |did_remap|.
  uint32_t num_sample_events_mapped;

  // Whether address remapping was enabled during event parsing.
  bool did_remap;
};

class PerfParser : public PerfReader {
 public:
  PerfParser();
  ~PerfParser();

  struct Options {
    // For synthetic address mapping.
    bool do_remap = false;
    // Set this flag to discard non-sample events that don't have any associated
    // sample events. e.g. MMAP regions with no samples in them.
    bool discard_unused_events = false;
    // When mapping perf sample events, at least this percentage of them must be
    // successfully mapped in order for ProcessEvents() to return true.
    // By default, most samples must be properly mapped in order for sample
    // mapping to be considered successful.
    float sample_mapping_percentage_threshold = 95.0f;
  };

  // Constructor that takes in options at PerfParser creation time.
  explicit PerfParser(const Options& options);

  // Pass in a struct containing various options.
  void set_options(const Options& options);

  // Gets parsed event/sample info from raw event data.
  bool ParseRawEvents();

  const std::vector<ParsedEvent>& parsed_events() const {
    return parsed_events_;
  }

  // Returns an array of pointers to |parsed_events_| sorted by sample time.
  // The first time this is called, it will create the sorted array.
  const std::vector<ParsedEvent*>& GetEventsSortedByTime() const {
    return parsed_events_sorted_by_time_;
  }

  const PerfEventStats& stats() const {
    return stats_;
  }

 protected:
  // Defines a type for a pid:tid pair.
  typedef std::pair<uint32_t, uint32_t> PidTid;

  // Sort |parsed_events_| by time, storing the results in
  // |parsed_events_sorted_by_time_|.
  void SortParsedEvents();

  // Used for processing events.  e.g. remapping with synthetic addresses.
  bool ProcessEvents();
  bool MapMmapEvent(struct mmap_event* event, uint64_t id);
  bool MapForkEvent(const struct fork_event& event);
  bool MapCommEvent(const struct comm_event& event);

  // Create a process mapper for a process. Optionally pass in a parent pid
  // |ppid| from which to copy mappings.
  void CreateProcessMapper(uint32_t pid, uint32_t ppid = -1);

  // Does a sample event remap and then returns DSO name and offset of sample.
  bool MapSampleEvent(ParsedEvent* parsed_event);

  void ResetAddressMappers();

  std::vector<ParsedEvent> parsed_events_;
  std::vector<ParsedEvent*> parsed_events_sorted_by_time_;

  Options options_;   // Store all option flags as one struct.

  std::map<uint32_t, AddressMapper*> process_mappers_;

  // Maps pid/tid to commands.
  std::map<PidTid, const string*> pidtid_to_comm_map_;

  // A set to store the actual command strings.
  std::set<string> commands_;

  PerfEventStats stats_;

  // A set of unique DSOs that may be referenced by multiple events.
  std::set<DSOInfo> dso_set_;

 private:
  // Calls MapIPAndPidAndGetNameAndOffset() on the callchain of a sample event.
  bool MapCallchain(const struct ip_event& event,
                    uint64_t original_event_addr,
                    struct ip_callchain* callchain,
                    ParsedEvent* parsed_event);

  // Trims the branch stack for null entries and calls
  // MapIPAndPidAndGetNameAndOffset() on each entry.
  bool MapBranchStack(const struct ip_event& event,
                      struct branch_stack* branch_stack,
                      ParsedEvent* parsed_event);

  // This maps a sample event and returns the mapped address, DSO name, and
  // offset within the DSO.  This is a private function because the API might
  // change in the future, and we don't want derived classes to be stuck with an
  // obsolete API.
  bool MapIPAndPidAndGetNameAndOffset(
      uint64_t ip,
      uint32_t pid,
      uint64_t* new_ip,
      ParsedEvent::DSOAndOffset* dso_and_offset);

  DISALLOW_COPY_AND_ASSIGN(PerfParser);
};

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_PERF_PARSER_H_
