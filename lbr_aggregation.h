#ifndef AUTOFDO_LBR_AGGREGATION_H_
#define AUTOFDO_LBR_AGGREGATION_H_

#include <string>
#include <utility>

#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/strings/str_format.h"

namespace devtools_crosstool_autofdo {
struct BinaryAddressBranch {
  uint64_t from, to;
  template <typename H>
  friend H AbslHashValue(H h, const BinaryAddressBranch &b) {
    return H::combine(std::move(h), b.from, b.to);
  }
  bool operator==(const BinaryAddressBranch &b) const {
    return from == b.from && to == b.to;
  }

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const BinaryAddressBranch &b) {
    absl::Format(&sink, "0x%016x->0x%016x", b.from, b.to);
  }
};

struct BinaryAddressFallthrough {
  uint64_t from, to;
  template <typename H>
  friend H AbslHashValue(H h, const BinaryAddressFallthrough &f) {
    return H::combine(std::move(h), f.from, f.to);
  }
  bool operator==(const BinaryAddressFallthrough &f) const {
    return from == f.from && to == f.to;
  }
};

struct LbrAggregation {
  int64_t GetNumberOfBranchCounters() const {
    return absl::c_accumulate(
        branch_counters, 0,
        [](int64_t cnt, const auto &v) { return cnt + v.second; });
  }

  static const uint64_t kInvalidAddress = static_cast<uint64_t>(-1);

  absl::flat_hash_map<BinaryAddressBranch, int64_t> branch_counters;
  absl::flat_hash_map<BinaryAddressFallthrough, int64_t> fallthrough_counters;
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LBR_AGGREGATION_H_
