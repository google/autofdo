#include "branch_frequencies_autofdo_sample.h"

#include <sys/types.h>

#include <cstdint>
#include <ios>
#include <iterator>
#include <optional>
#include <utility>

#include "binary_address_branch.h"
#include "branch_frequencies.h"
#include "sample_reader.h"
#include "base/logging.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"

namespace devtools_crosstool_autofdo {

namespace {
// Gets the total count of branch samples targeting each binary address.
absl::btree_map<int64_t, int64_t> GetIncomingBranchWeights(
    const BranchFrequencies& branch_frequencies) {
  absl::btree_map<int64_t, int64_t> weights;
  for (const auto& [branch, count] : branch_frequencies.taken_branch_counters) {
    // We want to consider the targets of branches from outside our address
    // space, but not the targets of branches *to* outside our address space.
    if (branch.to == kInvalidBinaryAddress) continue;

    weights[branch.to] += count;
  }
  return weights;
}

// Finds the first, if any, control-flow-affecting instruction in the half-open
// binary address interval [start, end).
std::optional<int64_t> FirstControlFlowInstruction(
    int64_t start, int64_t end,
    absl::FunctionRef<absl::StatusOr<bool>(int64_t address)>
        may_affect_control_flow_callback,
    int instruction_size) {
  // Iterate through the instruction addresses between start and end looking for
  // an address that can affect control flow.
  for (int64_t address = start; address < end; address += instruction_size) {
    if (may_affect_control_flow_callback(address).value_or(true))
      return address;
  }
  return std::nullopt;
}

// Returns the mapped value for `key` if present in `map`. Otherwise, returns
// `default_value`.
template <typename Map>
typename Map::mapped_type ValueOr(const Map& map, const typename Map::key_type& key,
                         typename Map::mapped_type default_value) {
  auto found = map.find(key);
  return found == map.end() ? default_value : found->second;
}

// Converts a map of binary address taken branch counts to a map of symbol
// offset taken branch counts
BranchCountMap ToBranchCountMap(
    const absl::flat_hash_map<BinaryAddressBranch, int64_t> taken_branch_counts,
    absl::FunctionRef<int64_t(int64_t)> binary_address_to_symbol_offset) {
  BranchCountMap branch_counts;
  for (const auto& [branch, count] : taken_branch_counts) {
    // Only consider branches within the valid address space.
    if (branch.from == kInvalidBinaryAddress ||
        branch.to == kInvalidBinaryAddress)
      continue;

    Branch symbol_offset_branch =
        std::make_pair(binary_address_to_symbol_offset(branch.from),
                       binary_address_to_symbol_offset(branch.to));
    branch_counts[symbol_offset_branch] += count;
  }
  return branch_counts;
}

// Generates a map of symbol offset range execution counts from incoming branch
// weights and not-taken branch counts. The input maps encode, for each binary
// address, the total weight of branches targeting the address and the number of
// not-taken branch samples for the address.
RangeCountMap ToRangeCountMap(
    const absl::btree_map<int64_t, int64_t>& incoming_branch_weights,
    const absl::flat_hash_map<BinaryAddressNotTakenBranch, int64_t>&
        not_taken_branch_counts,
    absl::FunctionRef<absl::StatusOr<bool>(int64_t address)>
        may_affect_control_flow,
    absl::FunctionRef<int64_t(int64_t)> binary_address_to_symbol_offset,
    int instruction_size) {
  RangeCountMap range_counts;

  // The number of times an address range is executed is the sum of
  // incoming fallthrough weight from the preceding range and incoming branch
  // weight. At the start and end of each iteration, `range_count` contains the
  // fallthrough weight from the previous range to the next.
  int64_t range_weight = 0;

  // Addresses with nonzero incoming branch weights are the beginning of a
  // range. For each of these addresses, determine how far the range extends and
  // propagate the weight to subsequent ranges.
  for (auto it = incoming_branch_weights.begin();
       it != incoming_branch_weights.end(); ++it) {
    int64_t range_start = it->first;

    // Account for the incoming branches.
    range_weight += it->second;

    // For the final range, we pick 0x1000 as a reasonable upper bound on the
    // range size since the basic block that contains the range is unlikely to
    // be larger than that size.
    const int64_t scan_end = std::next(it) == incoming_branch_weights.end()
                                 ? range_start + 0x1000
                                 : std::next(it)->first;

    while (range_weight > 0 && range_start < scan_end) {
      std::optional<int64_t> next_branch = FirstControlFlowInstruction(
          range_start, scan_end, may_affect_control_flow, instruction_size);

      // Since BinaryAddressRange is a half-open interval and symbol-offset
      // range is a closed interval, we need to get the address of the last
      // instruction *in the range*.
      int64_t last_in_range = next_branch.value_or(scan_end - instruction_size);
      Range range =
          std::make_pair(binary_address_to_symbol_offset(range_start),
                         binary_address_to_symbol_offset(last_in_range));
      range_counts[range] += range_weight;

      // If the range ends in a branch instruction, the fallthrough weight is
      // the number of not-taken samples for the instruction.
      if (next_branch.has_value()) {
        range_weight =
            ValueOr(not_taken_branch_counts,
                    BinaryAddressNotTakenBranch{
                        .address = static_cast<uint64_t>(*next_branch)},
                    0);
      }
      range_start = last_in_range + instruction_size;
    }
  }
  return range_counts;
}

}  // namespace

absl::StatusOr<AutofdoRangeSample> ConvertBranchFrequenciesToAutofdoRangeSample(
    const BranchFrequencies& branch_frequencies,
    absl::FunctionRef<absl::StatusOr<bool>(int64_t address)>
        may_affect_control_flow,
    absl::FunctionRef<int64_t(int64_t)> binary_address_to_symbol_offset,
    int instruction_size) {
  AutofdoRangeSample sample;
  sample.branch_counts =
      ToBranchCountMap(branch_frequencies.taken_branch_counters,
                       binary_address_to_symbol_offset);

  absl::btree_map<int64_t, int64_t> incoming_branch_weights =
      GetIncomingBranchWeights(branch_frequencies);

  sample.range_counts = ToRangeCountMap(
      incoming_branch_weights, branch_frequencies.not_taken_branch_counters,
      may_affect_control_flow, binary_address_to_symbol_offset,
      instruction_size);

  // absl::btree_map<Branch, int64_t> branch_counts;
  // for (const auto& [branch, count] : sample.branch_counts) {
  //   branch_counts[branch] += count;
  // }

  // absl::btree_map<Range, int64_t> range_counts;
  // for (const auto& [range, count] : sample.range_counts) {
  //   range_counts[range] += count;
  // }

  // LOG(INFO) << "@" << range_counts.size();
  // for (const auto& [range, count] : range_counts) {
  //   LOG(INFO) << absl::StrFormat("@%x-%x:%d", range.first, range.second, count);
  // }
  // LOG(INFO) << "@" << branch_counts.size();
  // for (const auto& [branch, count] : branch_counts) {
  //   LOG(INFO) << absl::StrFormat("@%x->%x:%d", branch.first, branch.second,
  //                                count);
  // }

  return sample;
}

}  // namespace devtools_crosstool_autofdo
