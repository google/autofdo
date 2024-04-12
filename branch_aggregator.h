#ifndef AUTOFDO_BRANCH_AGGREGATOR_H_
#define AUTOFDO_BRANCH_AGGREGATOR_H_

#include <cstdint>

#include "branch_aggregation.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/status/statusor.h"

namespace devtools_crosstool_autofdo {
// `BranchAggregator` is an abstraction around producing a `BranchAggregation`,
// making the source of the branch data (SPE, LBR) and profile (memtrace, perf)
// opaque to the user.
class BranchAggregator {
 public:
  virtual ~BranchAggregator() = default;

  // Gets the set of branch endpoint addresses (i.e. the set of addresses which
  // are either the source or target of a branch or fallthrough).
  virtual absl::StatusOr<absl::flat_hash_set<uint64_t>>
  GetBranchEndpointAddresses() = 0;

  // Returns a `BranchAggregation` for the binary mapped by
  // `binary_address_mapper`, or an `absl::Status` if a valid aggregation can't
  // be produced. Updates relevant Propeller statistics if aggregation succeeds;
  // otherwise, leaves `stats` in an undefined state.
  virtual absl::StatusOr<BranchAggregation> Aggregate(
      const BinaryAddressMapper& binary_address_mapper,
      PropellerStats& stats) = 0;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_BRANCH_AGGREGATOR_H_
