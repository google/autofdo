#ifndef AUTOFDO_LBR_AGGREGATOR_H_
#define AUTOFDO_LBR_AGGREGATOR_H_

#include "lbr_aggregation.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/status/statusor.h"
namespace devtools_crosstool_autofdo {
// `LbrAggregator` is an abstraction around producing an `LbrAggregation`,
// making the source of the aggregation (Perf data, memtrace, mock) opaque to
// the user of the aggregation.
class LbrAggregator {
 public:
  virtual ~LbrAggregator() = default;

  // Returns an `LbrAggregation` for the specified binary according to the given
  // options, or an `absl::Status` if a valid aggregation can't be produced.
  //
  // `AggregateLbrData` can fail for various reasons, depending on the
  // implementation.
  virtual absl::StatusOr<LbrAggregation> AggregateLbrData(
      const PropellerOptions& options, const BinaryContent& binary_content,
      PropellerStats& stats) = 0;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LBR_AGGREGATOR_H_
