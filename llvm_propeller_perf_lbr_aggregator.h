#ifndef AUTOFDO_LLVM_PROPELLER_PERF_LBR_AGGREGATOR_H_
#define AUTOFDO_LLVM_PROPELLER_PERF_LBR_AGGREGATOR_H_

#include <memory>
#include <utility>

#include "lbr_aggregation.h"
#include "lbr_aggregator.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/status/statusor.h"
namespace devtools_crosstool_autofdo {
// An implementation of `LbrAggregator` that builds an `LbrAggregation` from
// perf data containing LBR entries. The perf data can come from any
// `PerfDataProvider`, such as from a file, GFile, or mock.
class PerfLbrAggregator : public LbrAggregator {
 public:
  // PerfLbrAggregator is move-only; define the move operations and explicitly
  // delete the copy operations
  PerfLbrAggregator(PerfLbrAggregator&&) = default;
  PerfLbrAggregator& operator=(PerfLbrAggregator&&) = default;
  PerfLbrAggregator(const PerfLbrAggregator&) = delete;
  PerfLbrAggregator& operator=(const PerfLbrAggregator&) = delete;

  explicit PerfLbrAggregator(
      std::unique_ptr<PerfDataProvider> perf_data_provider)
      : perf_data_provider_(std::move(perf_data_provider)) {}

  absl::StatusOr<LbrAggregation> AggregateLbrData(
      const PropellerOptions& options, const BinaryContent& binary_content,
      PropellerStats& stats) override;

 private:
  // Checks that AggregatedLBR's source addresses are really branch, jmp, call
  // or return instructions and returns the resulting statistics.
  absl::StatusOr<PropellerStats::DisassemblyStats> CheckLbrAddress(
      const LbrAggregation& lbr_aggregation,
      const BinaryContent& binary_content);

  std::unique_ptr<PerfDataProvider> perf_data_provider_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_PERF_LBR_AGGREGATOR_H_
