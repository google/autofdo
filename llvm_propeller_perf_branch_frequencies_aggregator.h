#ifndef AUTOFDO_LLVM_PROPELLER_PERF_BRANCH_FREQUENCIES_AGGREGATOR_H_
#define AUTOFDO_LLVM_PROPELLER_PERF_BRANCH_FREQUENCIES_AGGREGATOR_H_

#include <memory>
#include <utility>

#include "branch_frequencies.h"
#include "branch_frequencies_aggregator.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/status/statusor.h"

namespace devtools_crosstool_autofdo {
// `PerfBranchFrequenciesAggregator` is an implementation of
// `BranchFrequenciesAggregator` that builds `BranchFrequencies` from perf data
// containing SPE entries. The perf data can come from any `PerfDataProvider`,
// such as from a file, GFile, or mock.
class PerfBranchFrequenciesAggregator : public BranchFrequenciesAggregator {
 public:
  explicit PerfBranchFrequenciesAggregator(
      std::unique_ptr<PerfDataProvider> perf_data_provider)
      : perf_data_provider_(std::move(perf_data_provider)) {}

  // PerfBranchFrequenciesAggregator is move-only.
  PerfBranchFrequenciesAggregator(PerfBranchFrequenciesAggregator&&) = default;
  PerfBranchFrequenciesAggregator& operator=(
      PerfBranchFrequenciesAggregator&&) = default;
  PerfBranchFrequenciesAggregator(const PerfBranchFrequenciesAggregator&) =
      delete;
  PerfBranchFrequenciesAggregator& operator=(
      const PerfBranchFrequenciesAggregator&) = delete;

  // Aggregates branch frequencies from perf data, may return an `absl::Status`
  // if the perf data can't be successfully parsed and aggregated (it doesn't
  // exist, is malformed, etc.).
  absl::StatusOr<BranchFrequencies> AggregateBranchFrequencies(
      const PropellerOptions& options, const BinaryContent& binary_content,
      PropellerStats& stats) override;

 private:
  std::unique_ptr<PerfDataProvider> perf_data_provider_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_PERF_BRANCH_FREQUENCIES_AGGREGATOR_H_
