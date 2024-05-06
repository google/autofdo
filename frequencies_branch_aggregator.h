#ifndef AUTOFDO_FREQUENCIES_BRANCH_AGGREGATOR_H_
#define AUTOFDO_FREQUENCIES_BRANCH_AGGREGATOR_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "binary_address_branch.h"
#include "branch_aggregation.h"
#include "branch_aggregator.h"
#include "branch_frequencies.h"
#include "branch_frequencies_aggregator.h"
#include "lazy_evaluator.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/base/attributes.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/status/statusor.h"

namespace devtools_crosstool_autofdo {
// An implementation of `BranchAggregator` that builds a `BranchAggregation`
// from aggregated branch frequency information.
class FrequenciesBranchAggregator : public BranchAggregator {
 public:
  // Constructs a `FrequenciesBranchAggregator` from `BranchFrequencies`
  // directly.
  explicit FrequenciesBranchAggregator(
      BranchFrequencies frequencies, PropellerStats stats = {},
      std::optional<int> instruction_size = std::nullopt);
  explicit FrequenciesBranchAggregator(BranchFrequencies frequencies,
                                       const BinaryContent& binary_content,
                                       PropellerStats stats = {});

  // Constructs a `FrequenciesBranchAggregator` from a
  // `BranchFrequenciesAggregator`.
  explicit FrequenciesBranchAggregator(
      std::unique_ptr<BranchFrequenciesAggregator> frequencies_aggregator,
      PropellerOptions options,
      const BinaryContent& binary_content ABSL_ATTRIBUTE_LIFETIME_BOUND);

  // FrequenciesBranchAggregator is move-only.
  FrequenciesBranchAggregator(FrequenciesBranchAggregator&&) = default;
  FrequenciesBranchAggregator& operator=(FrequenciesBranchAggregator&&) =
      default;
  FrequenciesBranchAggregator(const FrequenciesBranchAggregator&) = delete;
  FrequenciesBranchAggregator& operator=(const FrequenciesBranchAggregator&) =
      delete;

  absl::StatusOr<absl::flat_hash_set<uint64_t>> GetBranchEndpointAddresses()
      override;

  absl::StatusOr<BranchAggregation> Aggregate(
      const BinaryAddressMapper& binary_address_mapper,
      PropellerStats& stats) override;

 private:
  // The inputs needed to aggregate the branch frequencies.
  struct FrequencyInputs {
    std::unique_ptr<BranchFrequenciesAggregator> aggregator;
    const PropellerOptions options;
    const BinaryContent& binary_content;
  };

  // The outputs of aggregating the branch frequencies.
  struct FrequencyOutputs {
    absl::StatusOr<BranchFrequencies> frequencies;
    PropellerStats stats;
  };

  struct SampledWeights {
    int64_t incoming_weight = 0;
    int64_t outgoing_branch_weight = 0;
    std::optional<int64_t> actual_fallthrough_weight = std::nullopt;
  };

  // A map from basic block handle index to the block's sampled weights.
  using WeightsMap = absl::btree_map<int, SampledWeights>;

  // Computes the sampled weights for each block with an address in
  // `branch_frequencies`.
  WeightsMap ComputeBlockWeights(
      const BranchFrequencies& branch_frequencies,
      const BinaryAddressMapper& binary_address_mapper) const;

  // Accumulates the actual outgoing weights for each block whose final
  // instruction is in `not_taken_branches`.
  void AccumulateActualOutgoingWeights(
      const absl::flat_hash_map<BinaryAddressBranch, int64_t>& taken_branches,
      const absl::flat_hash_map<BinaryAddressNotTakenBranch, int64_t>&
          not_taken_branches,
      const BinaryAddressMapper& binary_address_mapper,
      WeightsMap& weights_map) const;

  // Accumulates the weights of incoming and outgoing branch edges for each
  // block with an address in `taken_branches`.
  void AccumulateBranchWeights(
      const absl::flat_hash_map<BinaryAddressBranch, int64_t>& taken_branches,
      const BinaryAddressMapper& binary_address_mapper,
      WeightsMap& weights_map) const;

  // Infers the weight of the fallthrough off the end of a basic block.
  int64_t InferFallthroughWeight(
      int64_t bb_handle_index, const SampledWeights& sampled_weights,
      const BinaryAddressMapper& binary_address_mapper) const;

  // Returns the index of the basic block that is the appropriate end of a
  // potential fallthrough (a fully-closed interval), or `std::nullopt` if there
  // is no appropriate end block between `from_bb_handle_index` to
  // `to_bb_handle_index`.
  //
  // A potential fallthrough may not be an actual valid fallthrough; for
  // example, a block's incoming edges may be randomly sampled more than its
  // outgoing edges, implying a fallthrough where there isn't one.
  //
  // The appropriate end block of a potential fallthrough is the block that
  // `from_bb_handle` can fall through to that most accurately attributes the
  // samples.
  //
  // If a potential fallthrough is valid, its end block (`to_bb_handle_index`)
  // is the appropriate one. Otherwise, the choice between valid end blocks is
  // subjective; to minimize blocks falsely attributed as hot, we choose the
  // start block's successor, if a valid end block.
  static std::optional<int> FindFallthroughEndBlock(
      int from_bb_handle_index, int to_bb_handle_index,
      const BinaryAddressMapper& binary_address_mapper);

  // Resolves a potential fallthrough by determining the appropriate end block
  // and updating the count of the resulting fallthrough if it is valid.
  void HandleFallthrough(int from_bb_handle_index, int to_bb_handle_index,
                         uint64_t weight,
                         const BinaryAddressMapper& binary_address_mapper,
                         absl::flat_hash_map<BinaryAddressFallthrough, int64_t>&
                             fallthroughs) const;

  // Infers the fallthrough edges and weights from the branch frequencies.
  absl::flat_hash_map<BinaryAddressFallthrough, int64_t> InferFallthroughs(
      const BranchFrequencies& branch_frequencies,
      const BinaryAddressMapper& binary_address_mapper) const;

  // Performs branch frequency aggregation, converting the inputs into the
  // outputs. This is a pure function, and it lives within
  // `FrequenciesBranchAggregator` to have access to `Frequency{In,Out}puts`.
  static FrequencyOutputs AggregateBranchFrequencies(FrequencyInputs inputs);

  // The fixed size of each instruction, if on a RISC architecture.
  std::optional<int> instruction_size_;
  // Lazily evaluates the branch frequency outputs, caching the result after the
  // first evaluation.
  LazyEvaluator<FrequencyOutputs(FrequencyInputs)> lazy_aggregator_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_FREQUENCIES_BRANCH_AGGREGATOR_H_
