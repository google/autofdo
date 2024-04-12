#include "frequencies_branch_aggregator.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "bb_handle.h"
#include "binary_address_branch.h"
#include "branch_aggregation.h"
#include "branch_frequencies.h"
#include "branch_frequencies_aggregator.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/base/attributes.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "llvm/TargetParser/Triple.h"
#include "base/status_macros.h"

namespace devtools_crosstool_autofdo {
namespace {
std::optional<int> GetInstructionSize(const BinaryContent& binary_content) {
  if (binary_content.object_file != nullptr &&
      binary_content.object_file->getArch() == llvm::Triple::aarch64) {
    return 4;
  }
  return std::nullopt;
}
}  // namespace

FrequenciesBranchAggregator::FrequenciesBranchAggregator(
    BranchFrequencies frequencies, PropellerStats stats,
    std::optional<int> instruction_size)
    : instruction_size_{instruction_size},
      lazy_aggregator_{FrequencyOutputs{.frequencies = std::move(frequencies),
                                        .stats = std::move(stats)}} {}

FrequenciesBranchAggregator::FrequenciesBranchAggregator(
    BranchFrequencies frequencies, const BinaryContent& binary_content,
    PropellerStats stats)
    : FrequenciesBranchAggregator(std::move(frequencies), std::move(stats),
                                  GetInstructionSize(binary_content)) {}

FrequenciesBranchAggregator::FrequenciesBranchAggregator(
    std::unique_ptr<BranchFrequenciesAggregator> frequencies_aggregator,
    PropellerOptions options,
    const BinaryContent& binary_content ABSL_ATTRIBUTE_LIFETIME_BOUND)
    : instruction_size_{GetInstructionSize(binary_content)},
      lazy_aggregator_{
          AggregateBranchFrequencies,
          FrequencyInputs{.aggregator = std::move(frequencies_aggregator),
                          .options = std::move(options),
                          .binary_content = std::move(binary_content)}} {}

absl::StatusOr<absl::flat_hash_set<uint64_t>>
FrequenciesBranchAggregator::GetBranchEndpointAddresses() {
  ASSIGN_OR_RETURN(BranchFrequencies frequencies,
                   lazy_aggregator_.Evaluate().frequencies);
  absl::flat_hash_set<uint64_t> endpoint_addresses;
  for (const auto& [branch, count] : frequencies.taken_branch_counters) {
    endpoint_addresses.insert(branch.from);
    endpoint_addresses.insert(branch.to);
  }
  for (const auto& [branch, count] : frequencies.not_taken_branch_counters) {
    endpoint_addresses.insert(branch.address);
  }
  return endpoint_addresses;
}

absl::StatusOr<BranchAggregation> FrequenciesBranchAggregator::Aggregate(
    const BinaryAddressMapper& binary_address_mapper, PropellerStats& stats) {
  ASSIGN_OR_RETURN(const BranchFrequencies& frequencies,
                   lazy_aggregator_.Evaluate().frequencies);

  absl::flat_hash_map<BinaryAddressFallthrough, int64_t> fallthrough_counts =
      InferFallthroughs(frequencies, binary_address_mapper);

  stats += lazy_aggregator_.Evaluate().stats;
  return BranchAggregation{
      .branch_counters = std::move(frequencies.taken_branch_counters),
      .fallthrough_counters = std::move(fallthrough_counts),
  };
}

FrequenciesBranchAggregator::FrequencyOutputs
FrequenciesBranchAggregator::AggregateBranchFrequencies(
    FrequencyInputs inputs) {
  FrequencyOutputs outputs;
  outputs.frequencies = inputs.aggregator->AggregateBranchFrequencies(
      inputs.options, inputs.binary_content, outputs.stats);
  return outputs;
}

FrequenciesBranchAggregator::WeightsMap
FrequenciesBranchAggregator::ComputeBlockWeights(
    const BranchFrequencies& branch_frequencies,
    const BinaryAddressMapper& binary_address_mapper) const {
  WeightsMap weights;
  AccumulateActualOutgoingWeights(branch_frequencies.taken_branch_counters,
                                  branch_frequencies.not_taken_branch_counters,
                                  binary_address_mapper, weights);
  AccumulateBranchWeights(branch_frequencies.taken_branch_counters,
                          binary_address_mapper, weights);
  return weights;
}

void FrequenciesBranchAggregator::AccumulateActualOutgoingWeights(
    const absl::flat_hash_map<BinaryAddressBranch, int64_t>& taken_branches,
    const absl::flat_hash_map<BinaryAddressNotTakenBranch, int64_t>&
        not_taken_branches,
    const BinaryAddressMapper& binary_address_mapper,
    WeightsMap& weights_map) const {
  // We can only determine if an instruction is the last in its block if we know
  // the size of the final instruction.
  if (!instruction_size_.has_value()) return;

  auto set_outgoing_weight = [&binary_address_mapper, &weights_map, this](
                                 uint64_t branch_address,
                                 int64_t outgoing_weight) {
    std::optional<int> handle_index =
        binary_address_mapper.FindBbHandleIndexUsingBinaryAddress(
            branch_address, BranchDirection::kFrom);
    if (!handle_index.has_value()) return;

    // If the branch is the last instruction in the block, set the weight.
    uint64_t last_instruction_address =
        binary_address_mapper.GetEndAddress(
            binary_address_mapper.bb_handles()[*handle_index]) -
        *instruction_size_;
    if (branch_address == last_instruction_address) {
      weights_map[*handle_index].actual_fallthrough_weight = outgoing_weight;
    }
  };

  // Initialize the actual outgoing weight as zero for blocks that end in taken
  // branches.
  for (const auto& [branch, _] : taken_branches) {
    set_outgoing_weight(branch.from, 0);
  }

  // Set the actual outgoing weight for blocks that end in not-taken branches.
  for (const auto& [branch, count] : not_taken_branches) {
    set_outgoing_weight(branch.address, count);
  }
}

void FrequenciesBranchAggregator::AccumulateBranchWeights(
    const absl::flat_hash_map<BinaryAddressBranch, int64_t>& taken_branches,
    const BinaryAddressMapper& binary_address_mapper,
    WeightsMap& weights_map) const {
  for (const auto& [branch, count] : taken_branches) {
    std::optional<int> from_bb_handle =
        binary_address_mapper.FindBbHandleIndexUsingBinaryAddress(
            branch.from, BranchDirection::kFrom);
    if (from_bb_handle.has_value()) {
      weights_map[*from_bb_handle].outgoing_branch_weight += count;
    }
    std::optional<int> to_bb_handle =
        binary_address_mapper.FindBbHandleIndexUsingBinaryAddress(
            branch.to, BranchDirection::kTo);
    if (to_bb_handle.has_value()) {
      weights_map[*to_bb_handle].incoming_weight += count;
    }
  }
}

int64_t FrequenciesBranchAggregator::InferFallthroughWeight(
    int64_t bb_handle_index, const SampledWeights& sampled_weights,
    const BinaryAddressMapper& binary_address_mapper) const {
  // If we don't know the actual fallthrough weight, assume that
  // `sum(incoming_weight) == sum(outgoing_branch_weights) +
  //   outgoing_fallthrough_weight`.
  return sampled_weights.actual_fallthrough_weight.value_or(
      sampled_weights.incoming_weight - sampled_weights.outgoing_branch_weight);
}

std::optional<int> FrequenciesBranchAggregator::FindFallthroughEndBlock(
    int from_bb_handle_index, int to_bb_handle_index,
    const BinaryAddressMapper& binary_address_mapper) {
  // Backward ranges and off-the-end ranges are invalid and have no
  // fallthrough.
  if (from_bb_handle_index >= to_bb_handle_index ||
      to_bb_handle_index >= binary_address_mapper.bb_handles().size()) {
    return std::nullopt;
  }

  if (binary_address_mapper.CanFallThrough(from_bb_handle_index,
                                           to_bb_handle_index)) {
    return to_bb_handle_index;
  }

  // TODO(hoekwater): investigate which end block is the most appropriate.
  // The current approach is "successor if valid"; some possible
  // alternatives are "longest valid fallthrough chain" and  "std::nullopt
  // if initial fallthrough is invalid."

  // The appropriate end block is `from_bb`'s successor, if that's a valid
  // fallthrough.
  int successor_index = from_bb_handle_index + 1;
  if (successor_index < binary_address_mapper.bb_handles().size() &&
      binary_address_mapper.CanFallThrough(from_bb_handle_index,
                                           successor_index)) {
    return successor_index;
  }
  return std::nullopt;
}

void FrequenciesBranchAggregator::HandleFallthrough(
    int from_bb_handle_index, int to_bb_handle_index, uint64_t weight,
    const BinaryAddressMapper& binary_address_mapper,
    absl::flat_hash_map<BinaryAddressFallthrough, int64_t>& fallthroughs)
    const {
  std::optional<int> fallthrough_end = FindFallthroughEndBlock(
      from_bb_handle_index, to_bb_handle_index, binary_address_mapper);

  if (fallthrough_end.has_value()) {
    uint64_t from_address = binary_address_mapper.GetAddress(
        binary_address_mapper.bb_handles()[from_bb_handle_index]);
    uint64_t to_address = binary_address_mapper.GetAddress(
        binary_address_mapper.bb_handles()[*fallthrough_end]);

    fallthroughs[{.from = from_address, .to = to_address}] += weight;
  }
}

absl::flat_hash_map<BinaryAddressFallthrough, int64_t>
FrequenciesBranchAggregator::InferFallthroughs(
    const BranchFrequencies& frequencies,
    const BinaryAddressMapper& binary_address_mapper) const {
  absl::flat_hash_map<BinaryAddressFallthrough, int64_t> fallthrough_counts;
  std::optional<std::pair<int, int64_t>> fallthrough_from;

  WeightsMap weights = ComputeBlockWeights(frequencies, binary_address_mapper);
  for (auto& [bb_handle_index, sampled_weights] : weights) {
    // Resolve any outstanding fallthrough before evaluating this block.
    if (fallthrough_from.has_value()) {
      // Propagate the fallthrough weight to the current block.
      if (binary_address_mapper.CanFallThrough(fallthrough_from->first,
                                               bb_handle_index)) {
        sampled_weights.incoming_weight += fallthrough_from->second;
      }
      HandleFallthrough(/*from_bb_handle_index=*/fallthrough_from->first,
                        /*to_bb_handle_index=*/bb_handle_index,
                        /*weight=*/fallthrough_from->second,
                        /*binary_address_mapper=*/binary_address_mapper,
                        /*fallthroughs=*/fallthrough_counts);
      fallthrough_from = std::nullopt;
    }

    int64_t fallthrough_weight = InferFallthroughWeight(
        bb_handle_index, sampled_weights, binary_address_mapper);
    if (fallthrough_weight > 0) {
      fallthrough_from = std::make_pair(bb_handle_index, fallthrough_weight);
    }
  }

  // Resolve any outstanding fallthrough.
  if (fallthrough_from.has_value()) {
    HandleFallthrough(/*from_bb_handle_index=*/fallthrough_from->first,
                      /*to_bb_handle_index=*/fallthrough_from->first + 1,
                      /*weight=*/fallthrough_from->second,
                      /*binary_address_mapper=*/binary_address_mapper,
                      /*fallthroughs=*/fallthrough_counts);
  }
  return fallthrough_counts;
}
}  // namespace devtools_crosstool_autofdo
