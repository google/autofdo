#ifndef AUTOFDO_BRANCH_FREQUENCIES_AGGREGATOR_H_
#define AUTOFDO_BRANCH_FREQUENCIES_AGGREGATOR_H_

#include "branch_frequencies.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/status/statusor.h"

namespace devtools_crosstool_autofdo {
// `BranchFrequenciesAggregator` is an abstraction around producing
// `BranchFrequencies`, making the source of the frequency information opaque to
// the user.
class BranchFrequenciesAggregator {
 public:
  virtual ~BranchFrequenciesAggregator() = default;

  // Returns `BranchFrequencies` for the specified binary according to the given
  // options, or an `absl::Status` if valid branch frequencies can't be
  // produced.
  virtual absl::StatusOr<BranchFrequencies> AggregateBranchFrequencies(
      const PropellerOptions& options, const BinaryContent& binary_content,
      PropellerStats& stats) = 0;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_BRANCH_FREQUENCIES_AGGREGATOR_H_
