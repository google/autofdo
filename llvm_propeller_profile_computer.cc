#include "llvm_propeller_profile_computer.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "addr2cu.h"
#include "branch_aggregation.h"
#include "branch_aggregator.h"
#include "lbr_branch_aggregator.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_function_cluster_info.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_perf_lbr_aggregator.h"
#include "llvm_propeller_profile.h"
#include "llvm_propeller_program_cfg.h"
#include "llvm_propeller_program_cfg_builder.h"
#include "llvm_propeller_statistics.h"
#include "status_provider.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/StringRef.h"
#include "base/logging.h"
#include "base/status_macros.h"

namespace devtools_crosstool_autofdo {
namespace {
// Evaluates if Propeller options contain profiles that are specified as
// non-LBR.
bool ContainsNonLbrProfile(const PropellerOptions &options) {
  return absl::c_any_of(options.input_profiles(),
                        [](const InputProfile &profile) {
                          return profile.type() != PERF_LBR &&
                                 profile.type() != PROFILE_TYPE_UNSPECIFIED;
                        });
}

// Extracts the input profile names from Propeller options.
std::vector<std::string> ExtractProfileNames(const PropellerOptions &options) {
  std::vector<std::string> profile_names(options.profile_names().begin(),
                                         options.profile_names().end());

  profile_names.reserve(options.profile_names_size() +
                        options.input_profiles_size());
  absl::c_transform(options.input_profiles(), std::back_inserter(profile_names),
                    [](const InputProfile &profile) { return profile.name(); });

  return profile_names;
}
}  // namespace

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
  if (ContainsNonLbrProfile(options))
    return absl::InvalidArgumentError("non-LBR profile type");

  return Create(options, std::make_unique<GenericFilePerfDataProvider>(
                             /*file_names=*/ExtractProfileNames(options)));
}

absl::StatusOr<std::unique_ptr<PropellerProfileComputer>>
PropellerProfileComputer::Create(
    PropellerOptions options,
    std::unique_ptr<PerfDataProvider> perf_data_provider) {
  if (ContainsNonLbrProfile(options))
    return absl::InvalidArgumentError("non-LBR profile type");

  ASSIGN_OR_RETURN(std::unique_ptr<BinaryContent> binary_content,
                   GetBinaryContent(options.binary_name()));

  auto branch_aggregator = std::make_unique<LbrBranchAggregator>(
      std::make_unique<PerfLbrAggregator>(std::move(perf_data_provider)),
      options, *binary_content);

  return Create(options, std::move(branch_aggregator),
                std::move(binary_content));
}

absl::StatusOr<std::unique_ptr<PropellerProfileComputer>>
PropellerProfileComputer::Create(
    PropellerOptions options,
    std::unique_ptr<BranchAggregator> branch_aggregator,
    std::unique_ptr<BinaryContent> binary_content) {
  if (binary_content == nullptr) {
    ASSIGN_OR_RETURN(binary_content, GetBinaryContent(options.binary_name()));
  }

  return std::unique_ptr<PropellerProfileComputer>(new PropellerProfileComputer(
      options, std::move(branch_aggregator), std::move(binary_content)));
}

// "GetProgramCfg" steps:
//   1. BranchAggregator::AggregateBranchData
//   2. Creates a `BinaryAddressMapper`.
//   3. ProgramCfgBuilder::Build
absl::StatusOr<std::unique_ptr<ProgramCfg>>
PropellerProfileComputer::GetProgramCfg(
    DefaultStatusProvider *status_provider) {
  ASSIGN_OR_RETURN(absl::flat_hash_set<uint64_t> unique_addresses,
                   branch_aggregator_->GetBranchEndpointAddresses());

  if (status_provider) status_provider->SetProgress(50);

  ASSIGN_OR_RETURN(std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
                   BuildBinaryAddressMapper(options_, *binary_content_, stats_,
                                            &unique_addresses));

  if (status_provider) status_provider->SetProgress(60);
  ASSIGN_OR_RETURN(
      BranchAggregation branch_aggregation,
      branch_aggregator_->Aggregate(*binary_address_mapper, stats_));

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
          .Build(branch_aggregation, std::move(binary_content_->file_content),
                 addr2cu.get());
  if (!program_cfg.ok()) return program_cfg.status();

  if (status_provider) status_provider->SetDone();
  return program_cfg;
}

}  // namespace devtools_crosstool_autofdo
