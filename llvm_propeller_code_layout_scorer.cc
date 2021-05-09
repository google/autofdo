#include "llvm_propeller_code_layout_scorer.h"

#include "llvm_propeller_options.pb.h"

namespace devtools_crosstool_autofdo {

namespace {
uint32_t safe_multiply(uint32_t a, uint32_t b, uint32_t c = 1u) {
  if (a == 0 || b == 0 || c == 0) return 0;
  uint32_t result = a * b * c;
  CHECK(c == result / a / b) << "Integer overflow when multiplying " << a << "*"
                             << b << "*" << c << "!";
  return result;
}
}  // namespace

// The original ext-tsp score calculation is described as follows.
// 1- If edge is a fallthrough:
//      edge.weight_ * fallthrough_weight
// 2- If edge is a forward jump:
//      edge.weight_ * forward_jump_weight *
//             (1 - src_sink_distance / forward_jump_distance)
// 3- If edge is a backward jump:
//      edge.weight_ * backward_jump_weight *
//             (1 - src_sink_distance / backward_jump_distance)
// In order to use integers instead of floats, we scale each calculation by
// forward_jump_distance * backward_jump_distance, resuling in the scaled score
// formulas below:
// 1- If edge is a fallthrough:
//      edge.weight_ * scaled_fallthrough_weight
// 2- If edge is a forward jump:
//      edge.weight_ * scaled_forward_jump_weight *
//             (forward_jump_distance - src_sink_distance)
// 3- If edge is a backward jump:
//      edge.weight_ * scaled_backward_jump_weight *
//             (backward_jump_distance - src_sink_distance)
// Where scaled_fallthrough_weight, scaled_forward_jump_weight, and
// scaled_backward_jump_weight are calculated as in the constructor below.
PropellerCodeLayoutScorer::PropellerCodeLayoutScorer(
    const PropellerCodeLayoutParameters &params)
    : code_layout_params_(params),
      scaled_fallthrough_weight_(
          safe_multiply(params.fallthrough_weight(),
                        std::max(params.forward_jump_distance(), 1u),
                        std::max(params.backward_jump_distance(), 1u))),
      scaled_forward_jump_weight_(
          safe_multiply(params.forward_jump_weight(),
                        std::max(params.backward_jump_distance(), 1u))),
      scaled_backward_jump_weight_(
          safe_multiply(params.backward_jump_weight(),
                        std::max(params.forward_jump_distance(), 1u))) {
  // Guard against overflow in GetEdgeScore.
  safe_multiply(scaled_forward_jump_weight_,
                std::max(params.forward_jump_distance(), 1u));
  safe_multiply(scaled_backward_jump_weight_,
                std::max(params.backward_jump_distance(), 1u));
}

// Returns the score for one edge, given its source to sink direction and
// distance in the layout.
uint64_t PropellerCodeLayoutScorer::GetEdgeScore(
    const CFGEdge &edge, int64_t src_sink_distance) const {
  // Approximate callsites to be in the middle of the source basic block.
  if (edge.IsCall()) src_sink_distance += edge.src_->size_ / 2;

  if (edge.IsReturn()) src_sink_distance += edge.sink_->size_ / 2;

  if (src_sink_distance == 0 && edge.info_ == CFGEdge::DEFAULT)
    return edge.weight_ * scaled_fallthrough_weight_;

  uint64_t absolute_src_sink_distance =
      static_cast<uint64_t>(std::abs(src_sink_distance));
  if (src_sink_distance > 0 &&
      absolute_src_sink_distance < code_layout_params_.forward_jump_distance())
    return edge.weight_ * scaled_forward_jump_weight_ *
           (code_layout_params_.forward_jump_distance() -
            absolute_src_sink_distance);

  if (src_sink_distance < 0 &&
      absolute_src_sink_distance < code_layout_params_.backward_jump_distance())
    return edge.weight_ * scaled_backward_jump_weight_ *
           (code_layout_params_.backward_jump_distance() -
            absolute_src_sink_distance);
  return 0;
}

}  // namespace devtools_crosstool_autofdo
