#include "llvm_propeller_code_layout_scorer.h"

#include <algorithm>
#include <cstdlib>

#include "llvm_propeller_options.pb.h"

namespace devtools_crosstool_autofdo {

// The ext-tsp score calculation [1] is described as follows:
// 1- If edge is a fallthrough:
//      edge.weight_ * fallthrough_weight
// 2- If edge is a forward jump:
//      edge.weight_ * forward_jump_weight *
//             (1 - src_sink_distance / forward_jump_distance)
// 3- If edge is a backward jump:
//      edge.weight_ * backward_jump_weight *
//             (1 - src_sink_distance / backward_jump_distance)
//
// [1] Newell A, Pupyrev S. Improved basic block reordering.
//     IEEE Transactions on Computers. 2020 Mar 30;69(12):1784-94.
PropellerCodeLayoutScorer::PropellerCodeLayoutScorer(
    const PropellerCodeLayoutParameters &params)
    : code_layout_params_(params) {}

// Returns the score for one edge, given its source to sink direction and
// distance in the layout.
double PropellerCodeLayoutScorer::GetEdgeScore(const CFGEdge &edge,
                                               int src_sink_distance) const {
  // Approximate callsites to be in the middle of the source basic block.
  if (edge.IsCall()) src_sink_distance += edge.src()->size() / 2;

  if (edge.IsReturn()) src_sink_distance += edge.sink()->size() / 2;

  if (src_sink_distance == 0 && edge.IsBranchOrFallthrough())
    return edge.weight() * code_layout_params_.fallthrough_weight();

  double absolute_src_sink_distance =
      static_cast<double>(std::abs(src_sink_distance));
  if (src_sink_distance > 0 &&
      absolute_src_sink_distance < code_layout_params_.forward_jump_distance())
    return edge.weight() * code_layout_params_.forward_jump_weight() *
           (1.0 - absolute_src_sink_distance /
                    code_layout_params_.forward_jump_distance());

  if (src_sink_distance < 0 &&
      absolute_src_sink_distance < code_layout_params_.backward_jump_distance())
    return edge.weight() * code_layout_params_.backward_jump_weight() *
           (1.0 - absolute_src_sink_distance /
                    code_layout_params_.backward_jump_distance());
  return 0;
}

}  // namespace devtools_crosstool_autofdo
