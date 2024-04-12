#include "lbr_branch_aggregator.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "branch_aggregation.h"
#include "lbr_aggregation.h"
#include "lbr_aggregator.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/base/attributes.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "base/status_macros.h"

namespace devtools_crosstool_autofdo {

LbrBranchAggregator::LbrBranchAggregator(LbrAggregation aggregation,
                                         PropellerStats stats)
    : lazy_aggregator_{/*output=*/LbrAggregationOutputs{
          .aggregation = std::move(aggregation), .stats = std::move(stats)}} {}

LbrBranchAggregator::LbrBranchAggregator(
    std::unique_ptr<LbrAggregator> aggregator, PropellerOptions options,
    const BinaryContent& binary_content ABSL_ATTRIBUTE_LIFETIME_BOUND)
    : lazy_aggregator_{/*adapter=*/AggregateLbrData, /*inputs=*/
                       LbrAggregationInputs{.aggregator = std::move(aggregator),
                                            .options = std::move(options),
                                            .binary_content = binary_content}} {
}

absl::StatusOr<absl::flat_hash_set<uint64_t>>
LbrBranchAggregator::GetBranchEndpointAddresses() {
  ASSIGN_OR_RETURN(const LbrAggregation& aggregation,
                   lazy_aggregator_.Evaluate().aggregation);
  absl::flat_hash_set<uint64_t> unique_addresses;
  for (const auto& [branch, count] : aggregation.branch_counters) {
    unique_addresses.insert(branch.from);
    unique_addresses.insert(branch.to);
  }
  for (const auto& [fallthrough, count] : aggregation.fallthrough_counters) {
    unique_addresses.insert(fallthrough.from);
    unique_addresses.insert(fallthrough.to);
  }
  return unique_addresses;
}

absl::StatusOr<BranchAggregation> LbrBranchAggregator::Aggregate(
    const BinaryAddressMapper& binary_address_mapper, PropellerStats& stats) {
  ASSIGN_OR_RETURN(const LbrAggregation& aggregation,
                   lazy_aggregator_.Evaluate().aggregation);

  stats += lazy_aggregator_.Evaluate().stats;
  return BranchAggregation{
      .branch_counters = aggregation.branch_counters,
      .fallthrough_counters = aggregation.fallthrough_counters};
}

LbrBranchAggregator::LbrAggregationOutputs
LbrBranchAggregator::AggregateLbrData(LbrAggregationInputs inputs) {
  LbrAggregationOutputs outputs;
  outputs.aggregation = inputs.aggregator->AggregateLbrData(
      inputs.options, inputs.binary_content, outputs.stats);
  return outputs;
}

}  // namespace devtools_crosstool_autofdo
