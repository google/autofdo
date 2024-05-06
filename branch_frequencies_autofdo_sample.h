#ifndef AUTOFDO_BRANCH_FREQUENCIES_AUTOFDO_SAMPLE_H_
#define AUTOFDO_BRANCH_FREQUENCIES_AUTOFDO_SAMPLE_H_

#include <cstdint>

#include "branch_frequencies.h"
#include "sample_reader.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/status/statusor.h"
namespace devtools_crosstool_autofdo {

// An AutoFDO profile sample made up of counts for braches and symbol offset
// ranges.
struct AutofdoRangeSample {
  // The number of times each branch (identified by the symbol offset of its
  // source and destination) was sampled.
  BranchCountMap branch_counts;
  // The number of times each range (the closed interval of symbol offsets
  // [first, second]) was sampled.
  RangeCountMap range_counts;
};

// Converts `branch_frequencies` to an AutoFDO range sample.
//
// Like `LbrAggregation`, the addresses in `BranchFrequencies` are static
// virtual addresses (binary addresses), so we need to convert them to symbol
// offsets, inverse of `SymbolMap::get_static_vaddr`.
absl::StatusOr<AutofdoRangeSample> ConvertBranchFrequenciesToAutofdoRangeSample(
    const BranchFrequencies& branch_frequencies,
    absl::FunctionRef<absl::StatusOr<bool>(int64_t address)>
        may_affect_control_flow,
    absl::FunctionRef<int64_t(int64_t)> binary_address_to_symbol_offset,
    int instruction_size);

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_BRANCH_FREQUENCIES_AUTOFDO_SAMPLE_H_
