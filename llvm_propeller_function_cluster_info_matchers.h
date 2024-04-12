#ifndef AUTOFDOLLVM_PROPELLER_FUNCTION_CLUSTER_INFO_MATCHERS_H_
#define AUTOFDOLLVM_PROPELLER_FUNCTION_CLUSTER_INFO_MATCHERS_H_

#include "llvm_propeller_function_cluster_info.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace devtools_crosstool_autofdo {

// Returns a matcher for `CFGScore` to match that its intra- and inter- scores
// are not more than `epsilon` away from `intra_score` and `inter_out_score`.
testing::Matcher<CFGScore> CfgScoreIsNear(double intra_score,
                                          double inter_out_score,
                                          double epsilon);

MATCHER_P(BbIdIs, bb_id_matcher,
          "has bb_id that " +
              testing::DescribeMatcher<int>(bb_id_matcher, negation)) {
  return testing::ExplainMatchResult(
      testing::Field("bb_id", &CFGNode::FullIntraCfgId::bb_id, bb_id_matcher),
      arg, result_listener);
}

MATCHER_P(HasFullBbIds, full_bb_ids_matcher,
          "has full_bb_ids that " +
              testing::DescribeMatcher<std::vector<CFGNode::FullIntraCfgId>>(
                  full_bb_ids_matcher, negation)) {
  return testing::ExplainMatchResult(
      testing::Field("full_bb_ids",
                     &FunctionClusterInfo::BBCluster::full_bb_ids,
                     full_bb_ids_matcher),
      arg, result_listener);
}

MATCHER_P(
    HasClusters, clusters_matcher,
    "has clusters that " +
        testing::DescribeMatcher<std::vector<FunctionClusterInfo::BBCluster>>(
            clusters_matcher, negation)) {
  return testing::ExplainMatchResult(
      testing::Field("clusters", &FunctionClusterInfo::clusters,
                     clusters_matcher),
      arg, result_listener);
}
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDOLLVM_PROPELLER_FUNCTION_CLUSTER_INFO_MATCHERS_H_
