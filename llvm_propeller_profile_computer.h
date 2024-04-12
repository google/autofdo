#ifndef AUTOFDO_LLVM_PROPELLER_PROFILE_COMPUTER_H_
#define AUTOFDO_LLVM_PROPELLER_PROFILE_COMPUTER_H_

#include <memory>
#include <utility>

#include "branch_aggregator.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_profile.h"
#include "llvm_propeller_program_cfg.h"
#include "llvm_propeller_statistics.h"
#include "status_provider.h"
#include "third_party/abseil/absl/status/statusor.h"

namespace devtools_crosstool_autofdo {

// Computes the `PropellerProfile` by reading the binary and profile.
// Example:
//    absl::StatusOr<std::unique_ptr<PropellerProfileComputer>>
//        profile_computer = Create(options);
//    absl::StatusOr<PropellerProfile>
//        profile = profile_computer->ComputeProfile();
class PropellerProfileComputer {
 public:
  // Creates a PropellerProfileComputer from a set of options. Requires that all
  // input profiles are of type PERF_LBR or PROFILE_TYPE_UNSPECIFIED.
  //
  // Note: Retain space between >> to allow parsing by SWIG interface generator.
  static absl::StatusOr<std::unique_ptr<PropellerProfileComputer> > Create(
      PropellerOptions options);

  // Creates a PropellerProfileComputer from a set of options and a perf data
  // provider. Requires that all input profiles are of type PERF_LBR or
  // PROFILE_TYPE_UNSPECIFIED.
  static absl::StatusOr<std::unique_ptr<PropellerProfileComputer> > Create(
      PropellerOptions options,
      std::unique_ptr<PerfDataProvider> perf_data_provider);

  // Creates a PropellerProfileComputer from an arbitrary branch aggregator and
  // binary content. If no binary content is provided, uses the binary specified
  // in `options`. The profiles specified in `options` are disregarded.
  static absl::StatusOr<std::unique_ptr<PropellerProfileComputer> > Create(
      PropellerOptions options,
      std::unique_ptr<BranchAggregator> branch_aggregator,
      std::unique_ptr<BinaryContent> binary_content = nullptr);

  // Returns the whole program CFG. Calculates a list of hot functions from
  // profiles and only creates CFGs for hot functions, this greatly reduces
  // memory consumption.
  absl::StatusOr<std::unique_ptr<ProgramCfg> > GetProgramCfg(
      DefaultStatusProvider *status_provider = nullptr);

  // Returns the propeller profile. Updates `map_profile_status` and
  // `code_layout_status` if they are not `nullptr`.
  absl::StatusOr<PropellerProfile> ComputeProfile(
      DefaultStatusProvider *map_profile_status = nullptr,
      DefaultStatusProvider *code_layout_status = nullptr);

  const PropellerStats &stats() const { return stats_; }
  void PrintStats() const;
  const BinaryContent &binary_content() const { return *binary_content_; }

 private:
  PropellerProfileComputer(const PropellerOptions &options,
                           std::unique_ptr<BranchAggregator> branch_aggregator,
                           std::unique_ptr<BinaryContent> binary_content)
      : options_(options),
        branch_aggregator_(std::move(branch_aggregator)),
        binary_content_(std::move(binary_content)) {}

  PropellerOptions options_;
  std::unique_ptr<BranchAggregator> branch_aggregator_;
  std::unique_ptr<BinaryContent> binary_content_;
  PropellerStats stats_;
};

}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_PROFILE_COMPUTER_H_
