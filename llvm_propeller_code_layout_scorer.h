#ifndef AUTOFDOLLVM_PROPELLER_CODE_LAYOUT_SCORER_H_
#define AUTOFDOLLVM_PROPELLER_CODE_LAYOUT_SCORER_H_

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_options.pb.h"

namespace devtools_crosstool_autofdo {

// This class is used to calculate the layout's extended TSP score as described
// in https://ieeexplore.ieee.org/document/9050435. Specifically, it calculates
// the contribution of a single edge with a given distance based on the
// specified code layout parameters.
class PropellerCodeLayoutScorer {
 public:
  explicit PropellerCodeLayoutScorer(
      const PropellerCodeLayoutParameters &params);
  double GetEdgeScore(const CFGEdge &edge, int src_sink_distance) const;
  const PropellerCodeLayoutParameters &code_layout_params() const {
    return code_layout_params_;
  }

 private:
  const PropellerCodeLayoutParameters code_layout_params_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDOLLVM_PROPELLER_CODE_LAYOUT_SCORER_H_
