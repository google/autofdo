#include "llvm_propeller_cfg.h"

#include <memory>
#include <sstream>

#include "llvm_propeller_cfg_matchers.h"
#include "llvm_propeller_cfg_testutil.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/types/span.h"

namespace devtools_crosstool_autofdo {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::IsEmpty;
using ::testing::Key;
using ::testing::Pointee;
using ::testing::UnorderedElementsAre;

TEST(LlvmPropellerCfg, CalculateNodeFreqsWithCallsAndReturns) {
  // This test sets up 3 functions, foo, bar, and baz, respectively with 3, 1,
  // and 1 CFG nodes. foo calls each of bar and baz from its entry node with
  // different frequencies. We verify that the node frequencies are correctly
  // calculated based on the call and return edges with highest frequencies.
  absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs =
      TestCfgBuilder(
          {.cfg_args =
               {{".section",
                 0,
                 "foo",
                 {{0x1000, 0, 0x10}, {0x1010, 1, 0x7}, {0x1020, 2, 0xa}},
                 {}},
                {".section", 1, "bar", {{0x2000, 0, 0x20}}, {}},
                {".section", 2, "baz", {{0x3000, 0, 0x10}}, {}}},
           .inter_edge_args = {{0, 0, 1, 0, 10, CFGEdge::Kind::kCall},
                               {1, 0, 0, 0, 8, CFGEdge::Kind::kRet},
                               {0, 0, 2, 0, 7, CFGEdge::Kind::kCall},
                               {2, 0, 0, 0, 9, CFGEdge::Kind::kRet}}})
          .Build();

  ASSERT_THAT(cfgs, UnorderedElementsAre(Key(0), Key(1), Key(2)));

  EXPECT_THAT(cfgs.at(0)->GetNodeFrequencies(), ElementsAre(10, 0, 0));
  EXPECT_THAT(cfgs.at(1)->GetNodeFrequencies(), ElementsAre(10));
  EXPECT_THAT(cfgs.at(2)->GetNodeFrequencies(), ElementsAre(9));
}

TEST(LlvmPropellerCfg, CalculateNodeFreqs_FunctionWithLoop) {
  // This test sets up a CFG with 4 nodes, where 2 nodes constitute a loop.
  // We verify that the node frequencies are appropriately calculated based on
  // the aggregation of in and out edge frequencies.
  absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs =
      TestCfgBuilder(
          {.cfg_args = {{".text",
                         0,
                         "foo",
                         {{0x1000, 0, 0x10},
                          {0x1010, 1, 0x7},
                          {0x1020, 2, 0xa},
                          {0x102a, 3, 0x4}},
                         {{0, 1, 10, CFGEdge::Kind::kBranchOrFallthough},
                          {1, 3, 95, CFGEdge::Kind::kBranchOrFallthough},
                          {3, 1, 100, CFGEdge::Kind::kBranchOrFallthough}}}}})
          .Build();

  ASSERT_THAT(cfgs, UnorderedElementsAre(Key(0)));

  EXPECT_THAT(cfgs.at(0)->GetNodeFrequencies(), ElementsAre(10, 110, 0, 100));
}

TEST(LlvmPropellerCfg, GetDotFormat) {
  absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs =
      TestCfgBuilder(
          {.cfg_args = {{".foo_section",
                         0,
                         "foo",
                         {{0x1000, 0, 0x10},
                          {0x1010, 1, 0x7},
                          {0x1020, 2, 0xa},
                          {0x102a, 3, 0x4}},
                         {{0, 1, 10, CFGEdge::Kind::kBranchOrFallthough},
                          {1, 3, 95, CFGEdge::Kind::kBranchOrFallthough},
                          {3, 1, 100, CFGEdge::Kind::kBranchOrFallthough}}}}})
          .Build();

  ASSERT_THAT(cfgs, UnorderedElementsAre(Key(0)));

  std::stringstream dot_format_os;
  cfgs.at(0)->WriteDotFormat(
      dot_format_os,
      // Assume the 0,1,3,2 layout.
      {{{0, 0}, 1}, {{1, 0}, 2}, {{2, 0}, 4}, {{3, 0}, 3}});
  EXPECT_EQ(dot_format_os.str(),
            "digraph {\n"
            "label=\"foo#0\"\n"
            "forcelabels=true;\n"
            "0 [xlabel=\"0#16#0\", color = \"black\" ];\n"
            "1 [xlabel=\"0#7#1\", color = \"black\" ];\n"
            "2 [xlabel=\"0#10#2\", color = \"black\" ];\n"
            "3 [xlabel=\"0#4#3\", color = \"black\" ];\n"
            "0 -> 1[ label=\"B#10\", color =\"red\"];\n"
            "1 -> 3[ label=\"B#95\", color =\"red\"];\n"
            "3 -> 1[ label=\"B#100\", color =\"black\"];\n"
            "}\n");
}

TEST(LlvmPropellerCfg, GetHotJoinNodes) {
  absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs =
      TestCfgBuilder(
          {.cfg_args = {{".foo_section",
                         0,
                         "foo",
                         {{0x1000, 0, 0x10, {true, false, false, false}},
                          {0x1010, 1, 0x7, {true, false, false, false}},
                          {0x1020, 2, 0xa, {false, false, false, false}},
                          {0x102a, 3, 0x4, {true, false, false, false}},
                          {0x1030, 4, 0x8, {false, true, false, false}}},
                         {{0, 1, 10, CFGEdge::Kind::kBranchOrFallthough},
                          {0, 3, 20, CFGEdge::Kind::kBranchOrFallthough},
                          {1, 2, 5, CFGEdge::Kind::kBranchOrFallthough},
                          {2, 0, 10, CFGEdge::Kind::kCall},
                          {2, 4, 5, CFGEdge::Kind::kBranchOrFallthough},
                          {1, 3, 30, CFGEdge::Kind::kBranchOrFallthough},
                          {3, 1, 40, CFGEdge::Kind::kBranchOrFallthough},
                          {3, 4, 25, CFGEdge::Kind::kBranchOrFallthough},
                          {4, 2, 30, CFGEdge::Kind::kRet}}}}})
          .Build();
  ASSERT_THAT(cfgs, UnorderedElementsAre(Key(0)));
  auto &foo_cfg = cfgs.at(0);
  EXPECT_THAT(foo_cfg->GetHotJoinNodes(/*hot_node_frequency_threshold=*/30,
                                       /*hot_edge_frequency_threshold=*/25),
              IsEmpty());
  EXPECT_THAT(foo_cfg->GetHotJoinNodes(30, 20), ElementsAre(3));
  EXPECT_THAT(foo_cfg->GetHotJoinNodes(30, 10), ElementsAre(1, 3));
  EXPECT_THAT(foo_cfg->GetHotJoinNodes(30, 5), ElementsAre(1, 3, 4));
  EXPECT_THAT(foo_cfg->GetHotJoinNodes(31, 5), ElementsAre(1, 3));
}

TEST(LlvmPropellerCfg, CloneCfg) {
  absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs =
      TestCfgBuilder(
          {.cfg_args = {{".foo_section",
                         0,
                         "foo",
                         {{0x1000, 0, 0x10},
                          {0x1010, 1, 0x7},
                          {0x1020, 2, 0xa},
                          {0x102a, 3, 0x4}},
                         {{0, 1, 10, CFGEdge::Kind::kBranchOrFallthough},
                          {1, 3, 95, CFGEdge::Kind::kBranchOrFallthough},
                          {3, 1, 100, CFGEdge::Kind::kBranchOrFallthough}}},
                        {".bar_section", 1, "bar", {{0x2000, 0, 0x20}}, {}}},
           .inter_edge_args = {{0, 3, 1, 0, 100, CFGEdge::Kind::kCall}}})
          .Build();
  ASSERT_THAT(cfgs, UnorderedElementsAre(Key(0), Key(1)));
  std::unique_ptr<ControlFlowGraph> clone_cfg = CloneCfg(*cfgs.at(0));
  EXPECT_THAT(
      clone_cfg,
      Pointee(CfgMatcher(
          CfgNodesMatcher(
              {AllOf(NodeIntraIdIs(CFGNode::IntraCfgId{0, 0}), NodeFreqIs(10)),
               AllOf(NodeIntraIdIs(CFGNode::IntraCfgId{1, 0}), NodeFreqIs(110)),
               AllOf(NodeIntraIdIs(CFGNode::IntraCfgId{2, 0}), NodeFreqIs(0)),
               AllOf(NodeIntraIdIs(CFGNode::IntraCfgId{3, 0}),
                     NodeFreqIs(100))}),
          CfgIntraEdgesMatcher(
              {IsCfgEdge(NodeIntraIdIs(CFGNode::IntraCfgId{0, 0}),
                         NodeIntraIdIs(CFGNode::IntraCfgId{1, 0}), 10,
                         CFGEdge::Kind::kBranchOrFallthough),
               IsCfgEdge(NodeIntraIdIs(CFGNode::IntraCfgId{1, 0}),
                         NodeIntraIdIs(CFGNode::IntraCfgId{3, 0}), 95,
                         CFGEdge::Kind::kBranchOrFallthough),
               IsCfgEdge(NodeIntraIdIs(CFGNode::IntraCfgId{3, 0}),
                         NodeIntraIdIs(CFGNode::IntraCfgId{1, 0}), 100,
                         CFGEdge::Kind::kBranchOrFallthough)}),
          CfgInterEdgesMatcher(absl::Span<const testing::Matcher<CFGEdge>>{}))));
}

TEST(LlvmPropellerCfg, GetNodeFrequencyStats) {
  absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs =
      TestCfgBuilder(
          {.cfg_args =
               {{ ".foo_section",
                 0,
                 "foo",
                 {{0x1000, 0, 0x10, {.CanFallThrough = true}},
                  {0x1010, 1, 0x7, {.CanFallThrough = true}},
                  {0x1020, 2, 0x0, {.CanFallThrough = true}},
                  {0x1020, 3, 0xa, {.IsEHPad = true, .CanFallThrough = true}},
                  {0x102a,
                   4,
                   0x4,
                   {.HasReturn = true, .CanFallThrough = false}}},
                 {{0, 2, 10, CFGEdge::Kind::kBranchOrFallthough},
                  {2, 3, 95, CFGEdge::Kind::kBranchOrFallthough},
                  {3, 2, 100, CFGEdge::Kind::kBranchOrFallthough},
                  {3, 4, 10, CFGEdge::Kind::kBranchOrFallthough}}}}})
          .Build();

  ASSERT_THAT(cfgs, UnorderedElementsAre(Key(0)));
  EXPECT_THAT(cfgs.at(0)->GetNodeFrequencyStats(), FieldsAre(4, 1, 1));
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
