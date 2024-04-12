#include "llvm_propeller_statistics.h"

#include "llvm_propeller_cfg.h"
#include "gtest/gtest.h"

namespace devtools_crosstool_autofdo {
namespace {

TEST(PropellerStatisticsTest, TotalEdgeWeightCreatedDoesntOverflow) {
  PropellerStats statistics = {
      .cfg_stats = {.total_edge_weight_by_kind =
                        {{CFGEdge::Kind::kBranchOrFallthough, 147121896},
                         {CFGEdge::Kind::kCall, 152487202},
                         {CFGEdge::Kind::kRet, 1902652788}}},
  };
  EXPECT_EQ(statistics.cfg_stats.total_edge_weight_created(), 2202261886);
}

}  // namespace
}  // namespace  devtools_crosstool_autofdo
