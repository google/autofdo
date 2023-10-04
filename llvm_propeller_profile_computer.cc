#include "llvm_propeller_profile_computer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "addr2cu.h"
#include "lbr_aggregation.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_function_cluster_info.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_profile.h"
#include "llvm_propeller_program_cfg.h"
#include "llvm_propeller_program_cfg_builder.h"
#include "llvm_propeller_statistics.h"
#include "mini_disassembler.h"
#include "perfdata_reader.h"
#include "status_provider.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/log/log.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInst.h"
#include "status_macros.h"

namespace devtools_crosstool_autofdo {

absl::StatusOr<PropellerProfile> PropellerProfileComputer::ComputeProfile(
    DefaultStatusProvider *map_profile_status,
    DefaultStatusProvider *code_layout_status) {
  ASSIGN_OR_RETURN(std::unique_ptr<ProgramCfg> program_cfg,
                   GetProgramCfg(map_profile_status));
  if (map_profile_status) map_profile_status->SetDone();

  absl::btree_map<llvm::StringRef, std::vector<FunctionClusterInfo>>
      cluster_info_by_section_name =
          GenerateLayoutBySection(*program_cfg, options_.code_layout_params(),
                                  stats_.code_layout_stats);

  if (code_layout_status) code_layout_status->SetDone();
  return PropellerProfile({.program_cfg = std::move(program_cfg),
                           .functions_cluster_info_by_section_name =
                               std::move(cluster_info_by_section_name)});
}

absl::StatusOr<std::unique_ptr<PropellerProfileComputer>>
PropellerProfileComputer::Create(PropellerOptions options) {
  return PropellerProfileComputer::Create(
      options, std::make_unique<FilePerfDataProvider>(std::vector<std::string>(
                   options.perf_names().begin(), options.perf_names().end())));
}

absl::StatusOr<std::unique_ptr<PropellerProfileComputer>>
PropellerProfileComputer::Create(
    PropellerOptions options,
    std::unique_ptr<PerfDataProvider> perf_data_provider) {
  ASSIGN_OR_RETURN(std::unique_ptr<BinaryContent> binary_content,
                   GetBinaryContent(options.binary_name()));

  return std::unique_ptr<PropellerProfileComputer>(new PropellerProfileComputer(
      options, std::move(perf_data_provider), std::move(binary_content)));
}

// "GetProgramCfg" steps:
//   1. ParsePerfData
//   2. Creates a `BinaryAddressMapper`.
//   3. ProgramCfgBuilder::Build
absl::StatusOr<std::unique_ptr<ProgramCfg>>
PropellerProfileComputer::GetProgramCfg(
    DefaultStatusProvider *status_provider) {
  ASSIGN_OR_RETURN(LbrAggregation lbr_aggregation, ParsePerfData());

  if (status_provider) status_provider->SetProgress(50);

  ASSIGN_OR_RETURN(std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
                   BuildBinaryAddressMapper(options_, *binary_content_, stats_,
                                            &lbr_aggregation));
  if (status_provider) status_provider->SetProgress(60);

  std::unique_ptr<devtools_crosstool_autofdo::Addr2Cu> addr2cu;
  if (options_.output_module_name()) {
    if (binary_content_->dwarf_context != nullptr) {
      addr2cu = std::make_unique<devtools_crosstool_autofdo::Addr2Cu>(
          *binary_content_->dwarf_context);
    } else {
      return absl::FailedPreconditionError(absl::StrFormat(
          "no DWARFContext is available for '%s'. Either because it does not "
          "have debuginfo, or '%s.dwp' does not exist.",
          options_.binary_name().c_str(), options_.binary_name().c_str()));
    }
  }
  absl::StatusOr<std::unique_ptr<ProgramCfg>> program_cfg =
      ProgramCfgBuilder(binary_address_mapper.get(), stats_)
          .Build(lbr_aggregation, std::move(binary_content_->file_content),
                 addr2cu.get());
  if (!program_cfg.ok()) return program_cfg.status();

  if (status_provider) status_provider->SetDone();
  return program_cfg;
}

// Parse perf data profile.
absl::StatusOr<LbrAggregation> PropellerProfileComputer::ParsePerfData() {
  std::string match_mmap_name = options_.binary_name();
  if (options_.has_profiled_binary_name()) {
    // If user specified "--profiled_binary_name", we use it.
    match_mmap_name = options_.profiled_binary_name();
  } else if (!options_.ignore_build_id()) {
    // Set match_mmap_name to "", so PerfDataReader::SelectPerfInfo auto
    // picks filename based on build-id, if build id is present; otherwise,
    // PerfDataReader::SelectPerfInfo uses options_.binary_name to match mmap
    // event file name.
    match_mmap_name = "";
  }
  LbrAggregation lbr_aggregation;

  while (true) {
    ASSIGN_OR_RETURN(std::optional<PerfDataProvider::BufferHandle> perf_data,
                     perf_data_provider_->GetNext());

    if (!perf_data.has_value()) break;

    std::string description = perf_data->description;
    LOG(INFO) << "Parsing " << description << " ...";
    absl::StatusOr<PerfDataReader> perf_data_reader = BuildPerfDataReader(
        std::move(*perf_data), binary_content_.get(), match_mmap_name);
    if (!perf_data_reader.ok()) {
      LOG(WARNING) << "Skipped profile " << description << ": "
                   << perf_data_reader.status();
      continue;
    }

    stats_.binary_mmap_num += perf_data_reader->binary_mmaps().size();
    ++stats_.perf_file_parsed;
    perf_data_reader->AggregateLBR(&lbr_aggregation);
  }
  stats_.br_counters_accumulated += lbr_aggregation.GetNumberOfBranchCounters();
  if (stats_.br_counters_accumulated <= 100)
    LOG(WARNING) << "Too few branch records in perf data.";
  if (!stats_.perf_file_parsed) {
    return absl::FailedPreconditionError(
        "No perf file is parsed, cannot proceed.");
  }

  ASSIGN_OR_RETURN(stats_.disassembly_stats, CheckLbrAddress(lbr_aggregation));
  return lbr_aggregation;
}

absl::StatusOr<PropellerStats::DisassemblyStats>
PropellerProfileComputer::CheckLbrAddress(
    const LbrAggregation &lbr_aggregation) {
  PropellerStats::DisassemblyStats result = {};

  ASSIGN_OR_RETURN(
      std::unique_ptr<MiniDisassembler> disassembler,
      MiniDisassembler::Create(binary_content_->object_file.get()));

  absl::flat_hash_map<int64_t, int64_t> counter_sum_by_source_address;
  for (const auto &[branch, counter] : lbr_aggregation.branch_counters) {
    if (branch.from == LbrAggregation::kInvalidAddress) continue;
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
