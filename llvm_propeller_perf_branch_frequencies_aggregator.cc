#include "llvm_propeller_perf_branch_frequencies_aggregator.h"

#include <optional>
#include <string>
#include <utility>

#include "branch_frequencies.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_statistics.h"
#include "perfdata_reader.h"
#include "base/logging.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "base/status_macros.h"

namespace devtools_crosstool_autofdo {
namespace {
// Determines the name of the profiled binary, which will be used to identify
// relevant Perf MMAP events. Returns the name of the binary, or it may return
// an empty string, which indicates that the build ID should be used instead.
std::string ResolveMmapName(const PropellerOptions &options) {
  std::string mmap_name = options.binary_name();
  if (options.has_profiled_binary_name()) {
    // If user specified "--profiled_binary_name", we use it.
    mmap_name = options.profiled_binary_name();
  } else if (!options.ignore_build_id()) {
    // Set mmap_name to "", so PerfDataReader::SelectPerfInfo auto
    // picks filename based on build-id, if build id is present; otherwise,
    // PerfDataReader::SelectPerfInfo uses options.binary_name to match mmap
    // event file name.
    mmap_name = "";
  }
  return mmap_name;
}
}  // namespace

absl::StatusOr<BranchFrequencies>
PerfBranchFrequenciesAggregator::AggregateBranchFrequencies(
    const PropellerOptions &options, const BinaryContent &binary_content,
    PropellerStats &stats) {
  const std::string match_mmap_name = ResolveMmapName(options);
  PropellerStats::ProfileStats &profile_stats = stats.profile_stats;
  BranchFrequencies frequencies;

  while (true) {
    ASSIGN_OR_RETURN(std::optional<PerfDataProvider::BufferHandle> perf_data,
                     perf_data_provider_->GetNext());
    if (!perf_data.has_value()) break;

    const std::string description = perf_data->description;
    LOG(INFO) << "Parsing " << description << " ...";
    absl::StatusOr<PerfDataReader> perf_data_reader = BuildPerfDataReader(
        std::move(*perf_data), &binary_content, match_mmap_name);
    if (!perf_data_reader.ok()) {
      LOG(WARNING) << "Skipped profile " << description << ": "
                   << perf_data_reader.status();
      continue;
    }

    profile_stats.binary_mmap_num += perf_data_reader->binary_mmaps().size();
    ++profile_stats.perf_file_parsed;
    RETURN_IF_ERROR(perf_data_reader->AggregateSpe(frequencies));
  }
  profile_stats.br_counters_accumulated +=
      frequencies.GetNumberOfTakenBranchCounters();
  if (profile_stats.br_counters_accumulated <= 100)
    LOG(WARNING) << "Too few branch records in perf data.";
  if (profile_stats.perf_file_parsed == 0) {
    return absl::FailedPreconditionError(
        "No perf file is parsed, cannot proceed.");
  }
  return frequencies;
}

}  // namespace devtools_crosstool_autofdo
