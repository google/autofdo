#ifndef AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_SCORER_H_
#define AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_SCORER_H_

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

  int64_t GetEdgeScore(const CFGEdge &edge, int64_t src_sink_distance) const;

  const PropellerCodeLayoutParameters &code_layout_params() const {
    return code_layout_params_;
  }
  uint32_t scalled_fallthrough_weight() const {
    return scaled_fallthrough_weight_;
  }
  uint32_t scaled_forward_jump_weight() const {
    return scaled_forward_jump_weight_;
  }
  uint32_t scaled_backward_jump_weight() const {
    return scaled_backward_jump_weight_;
  }

 private:
  const PropellerCodeLayoutParameters code_layout_params_;
  const uint32_t scaled_fallthrough_weight_;
  const uint32_t scaled_forward_jump_weight_;
  const uint32_t scaled_backward_jump_weight_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_SCORER_H_
