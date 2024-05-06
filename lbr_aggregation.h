#ifndef AUTOFDO_LBR_AGGREGATION_H_
#define AUTOFDO_LBR_AGGREGATION_H_

#include <cstdint>

#include "binary_address_branch.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"

namespace devtools_crosstool_autofdo {
// An aggregation of LBR-like data, which encodes a sequence of consecutive
// branches. `LbrAggregation` contains aggregated information about single
// branches and resulting fallthroughs. For example, for the following LBR
// entry: [
//   { from: 0x10, to: 0x20 },
//   { from: 0x40, to: 0x20 },
//   { from: 0x40, to: 0x20 },
// ], the resulting `LbrAggregation` encodes that the branch from 0x10 to 0x20
// was taken once, the branch from 0x40 to 0x20 was taken twice, and the
// fallthrough range from 0x20 to 0x40 was serially executed twice.
struct LbrAggregation {
  int64_t GetNumberOfBranchCounters() const {
    return absl::c_accumulate(
        branch_counters, 0,
        [](int64_t cnt, const auto &v) { return cnt + v.second; });
  }

  // A count of the number of times each branch was taken.
  absl::flat_hash_map<BinaryAddressBranch, int64_t> branch_counters;
  // A count of the number of times each fallthrough range (a fully-closed
  // interval) was serially taken. Given an instruction at binary address
  // `addr`, we can infer that the number of times the instruction was executed
  // is equal to the sum of counts for every fallthrough range that contains
  // `addr`.
  absl::flat_hash_map<BinaryAddressFallthrough, int64_t> fallthrough_counters;
};

}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LBR_AGGREGATION_H_
