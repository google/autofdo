#include "llvm_propeller_function_cluster_info_matchers.h"

#include "llvm_propeller_function_cluster_info.h"

namespace devtools_crosstool_autofdo {

testing::Matcher<CFGScore> CfgScoreIsNear(double intra_score,
                                          double inter_out_score,
                                          double epsilon) {
  return AllOf(testing::Field("intra_score", &CFGScore::intra_score,
                              testing::DoubleNear(intra_score, epsilon)),
               testing::Field("inter_out_score", &CFGScore::inter_out_score,
                              testing::DoubleNear(inter_out_score, epsilon)));
}

}  // namespace devtools_crosstool_autofdo
