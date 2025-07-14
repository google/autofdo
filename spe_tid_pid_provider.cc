#include "spe_tid_pid_provider.h"

#include <sys/types.h>

#include <cstdint>
#include <iterator>
#include <utility>

#include "base/logging.h"
#include "third_party/abseil/absl/base/casts.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/btree_set.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "quipper/arm_spe_decoder.h"
#include "quipper/perf_data_utils.h"
#include "google/protobuf/repeated_field.h"

namespace devtools_crosstool_autofdo {

SpeTidPidProvider::SpeTidPidProvider(
    const google::protobuf::RepeatedPtrField<quipper::PerfDataProto_PerfEvent>& events) {
  for (const quipper::PerfDataProto::PerfEvent& event : events) {
    if (event.has_time_conv_event()) {
      time_conv_event_ = event.time_conv_event();
    }

    auto record = [&](int pid, int tid, uint64_t timestamp) {
      if (pid <= 0 || tid <= 0) return;
      absl::btree_map<uint64_t, pid_t>& pids = tids_to_pids_[tid];
      // If the most recent entry is from this PID, don't bother adding it.
      if (!pids.empty() && pids.rbegin()->second == pid) return;
      VLOG(7) << absl::StrCat("tid = ", tid, ", timestamp = ", timestamp,
                              ", pid = ", pid, "\n");
      pids.insert(std::make_pair(timestamp, pid));
      pids_.insert(pid);
    };

    record(event.fork_event().pid(), event.fork_event().tid(),
           event.fork_event().fork_time_ns());
    if (event.sample_event().sample_time_ns() > 0) {
      record(event.sample_event().pid(), event.sample_event().tid(),
             event.sample_event().sample_time_ns());
    }
    const quipper::PerfDataProto::SampleInfo* info =
        quipper::GetSampleInfoForEvent(event);
    if (info != nullptr && info->sample_time_ns() > 0) {
      record(info->pid(), info->tid(), info->sample_time_ns());
    }
  }
}

uint64_t SpeTidPidProvider::SpeTimestampToPerfTimestamp(uint64_t cycles) const {
  // TSC to perf time conversion can be done as it is implemented in
  // http://google3/third_party/linux_tools/src/tools/perf/util/tsc.c?q=symbol%3A%5Cbtsc_to_perf_time%5Cb%20case%3Ayes
  if (time_conv_event_.cap_user_time_short()) {
    cycles = time_conv_event_.time_cycles() +
             ((cycles - time_conv_event_.time_cycles()) &
              time_conv_event_.time_mask());
  }

  uint64_t quot = cycles >> time_conv_event_.time_shift();
  uint64_t rem =
      cycles &
      ((absl::implicit_cast<uint64_t>(1) << time_conv_event_.time_shift()) - 1);
  return time_conv_event_.time_zero() + quot * time_conv_event_.time_mult() +
         ((rem * time_conv_event_.time_mult()) >>
          time_conv_event_.time_shift());
}

absl::StatusOr<int> SpeTidPidProvider::GetPid(
    const quipper::ArmSpeDecoder::Record& record) const {
  // An SPE context header must specify CONTEXTIDR_EL1 or CONTEXTIDR_EL2; if
  // neither are present, the context is still value-initialized and the context
  // ID is meaningless.
  if (!record.context.el1 && !record.context.el2) {
    if (pids_.size() == 1) {
       return *pids_.begin();
    }
    return absl::InvalidArgumentError(
        "Cannot get PID for an SPE event with an invalid context");
  }
  // If we can't resolve the actual PID, default to the TID.
  const int default_pid = record.context.id;

  auto found = tids_to_pids_.find(record.context.id);
  if (found == tids_to_pids_.end()) return default_pid;
  const absl::btree_map<uint64_t, pid_t>& pids = found->second;

  auto it = pids.upper_bound(SpeTimestampToPerfTimestamp(record.timestamp));

  if (it == pids.begin()) return default_pid;
  return std::prev(it)->second;
}

}  // namespace devtools_crosstool_autofdo
