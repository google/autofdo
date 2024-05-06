#include "llvm_propeller_perf_lbr_aggregator.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "binary_address_branch.h"
#include "lbr_aggregation.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_statistics.h"
#include "mini_disassembler.h"
#include "perfdata_reader.h"
#include "base/logging.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "llvm/MC/MCInst.h"
#include "base/status_macros.h"

namespace devtools_crosstool_autofdo {

absl::StatusOr<LbrAggregation> PerfLbrAggregator::AggregateLbrData(
    const PropellerOptions &options, const BinaryContent &binary_content,
    PropellerStats &stats) {
  PropellerStats::ProfileStats &profile_stats = stats.profile_stats;
  std::string match_mmap_name = options.binary_name();
  if (options.has_profiled_binary_name()) {
    // If user specified "--profiled_binary_name", we use it.
    match_mmap_name = options.profiled_binary_name();
  } else if (!options.ignore_build_id()) {
    // Set match_mmap_name to "", so PerfDataReader::SelectPerfInfo auto
    // picks filename based on build-id, if build id is present; otherwise,
    // PerfDataReader::SelectPerfInfo uses options.binary_name to match mmap
    // event file name.
    match_mmap_name = "";
  }
  LbrAggregation lbr_aggregation;

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
    ++stats.profile_stats.perf_file_parsed;
    perf_data_reader->AggregateLBR(&lbr_aggregation);
  }
  profile_stats.br_counters_accumulated +=
      lbr_aggregation.GetNumberOfBranchCounters();
  if (profile_stats.br_counters_accumulated <= 100)
    LOG(WARNING) << "Too few branch records in perf data.";
  if (!profile_stats.perf_file_parsed) {
    return absl::FailedPreconditionError(
        "No perf file is parsed, cannot proceed.");
  }

  ASSIGN_OR_RETURN(stats.disassembly_stats,
                   CheckLbrAddress(lbr_aggregation, binary_content));
  return lbr_aggregation;
}

absl::StatusOr<PropellerStats::DisassemblyStats>
PerfLbrAggregator::CheckLbrAddress(const LbrAggregation &lbr_aggregation,
                                   const BinaryContent &binary_content) {
  PropellerStats::DisassemblyStats result = {};

  ASSIGN_OR_RETURN(std::unique_ptr<MiniDisassembler> disassembler,
                   MiniDisassembler::Create(binary_content.object_file.get()));

  absl::flat_hash_map<int64_t, int64_t> counter_sum_by_source_address;
  for (const auto &[branch, counter] : lbr_aggregation.branch_counters) {
    if (branch.from == kInvalidBinaryAddress) continue;
    counter_sum_by_source_address[branch.from] += counter;
  }

  for (const auto &[address, counter] : counter_sum_by_source_address) {
    absl::StatusOr<llvm::MCInst> inst = disassembler->DisassembleOne(address);
    if (!inst.ok()) {
      result.could_not_disassemble.Increment(counter);
      LOG(WARNING) << absl::StrFormat(
          "not able to disassemble address: 0x%x with counter sum %d", address,
          counter);
      continue;
    }
    if (!disassembler->MayAffectControlFlow(*inst)) {
      result.cant_affect_control_flow.Increment(counter);
      LOG(WARNING) << absl::StrFormat(
          "not a potentially-control-flow-affecting "
          "instruction at address: "
          "0x%x with counter sum %d, instruction name: %s",
          address, counter, disassembler->GetInstructionName(*inst));
    } else {
      result.may_affect_control_flow.Increment(counter);
    }
  }

  return result;
}

}  // namespace devtools_crosstool_autofdo
