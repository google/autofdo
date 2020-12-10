#include "llvm_propeller_code_layout.h"

#include <memory>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_cfg.pb.h"
#include "llvm_propeller_mock_whole_program_info.h"
#include "llvm_propeller_node_chain_builder.h"
#include "llvm_propeller_options.h"
#include "llvm_propeller_whole_program_info.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/string_view.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

using ::devtools_crosstool_autofdo::CodeLayout;
using ::devtools_crosstool_autofdo::ControlFlowGraph;
using ::devtools_crosstool_autofdo::MockPropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::PropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::NodeChain;
using ::devtools_crosstool_autofdo::NodeChainBuilder;
using ::devtools_crosstool_autofdo::PropellerOptions;

static std::unique_ptr<MockPropellerWholeProgramInfo> GetTestWholeProgramInfo(
    const std::string &testdata_path) {
  const PropellerOptions options = {
      .perf_name = FLAGS_test_srcdir + testdata_path};
  auto whole_program_info = std::make_unique<
      devtools_crosstool_autofdo::MockPropellerWholeProgramInfo>(options);
  if (!whole_program_info->CreateCfgs()) {
    return nullptr;
  }
  return whole_program_info;
}

// Helper method to capture the ordinals of a chain's nodes in a vector.
static std::vector<uint64_t> GetOrderedNodeIds(
    const std::unique_ptr<NodeChain> &chain) {
  std::vector<uint64_t> node_ids;
  chain->VisitEachNodeRef(
      [&node_ids](auto &n) { node_ids.push_back(n.symbol_ordinal_); });
  return node_ids;
}

// This tests every step in NodeChainBuilder::BuildChains on a single CFG.
TEST(CodeLayoutTest, BuildChains) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_three_branches.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  EXPECT_EQ(whole_program_info->cfgs().size(), 1);
  const std::unique_ptr<ControlFlowGraph> &foo_cfg =
      whole_program_info->cfgs().at("foo");
  ASSERT_EQ(6, foo_cfg->nodes_.size());
  auto chain_builder = NodeChainBuilder(foo_cfg.get());

  chain_builder.InitNodeChains();

  auto &chains = chain_builder.chains();
  // Verify that there are 5 chains corresponding to the hot nodes.
  EXPECT_EQ(5, chains.size());
  // Verify that every chain has a single node.
  for (auto &chain_elem : chains) {
    EXPECT_THAT(GetOrderedNodeIds(chain_elem.second),
                testing::ElementsAreArray(
                    {chain_elem.second->delegate_node_->symbol_ordinal_}));
  }

  chain_builder.InitChainEdges();

  // Verify the number of in edges and out edges of every chain.
  struct {
    uint64_t chain_id;
    int out_edges_count;
    int in_edges_count;
  } expected_edge_counts[] = {
      {1, 1, 0}, {2, 1, 1}, {3, 1, 0}, {4, 0, 1}, {5, 0, 1}};
  for (const auto &chain_edge_count : expected_edge_counts) {
    EXPECT_EQ(chain_edge_count.out_edges_count,
              chains.at(chain_edge_count.chain_id)->out_edges_.size());
    EXPECT_EQ(chain_edge_count.in_edges_count,
              chains.at(chain_edge_count.chain_id)->in_edges_.size());
  }

  chain_builder.InitChainAssemblies();

  // Verify the number of chain assemblies.
  EXPECT_EQ(5, chain_builder.node_chain_assemblies().size());

  chain_builder.KeepMergingBestChains();

  // Verify that the chain assemblies is empty now.
  EXPECT_TRUE(chain_builder.node_chain_assemblies().empty());
  // Verify the constructed chains.
  EXPECT_EQ(2, chains.size());
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)),
              testing::ElementsAreArray({1, 2, 5}));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)),
              testing::ElementsAreArray({3, 4}));

  chain_builder.CoalesceChains();

  // Verify that the two chains are coalesced together.
  EXPECT_EQ(1, chains.size());
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)),
              testing::ElementsAreArray({1, 2, 5, 3, 4}));
}

TEST(CodeLayoutTest, FindOptimalFallthrough) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_conditional.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  EXPECT_EQ(whole_program_info->cfgs().size(), 1);
  auto layout_info = CodeLayout(whole_program_info->GetHotCfgs()).OrderAll();
  EXPECT_EQ(1, layout_info.size());
  EXPECT_NE(layout_info.find(1), layout_info.end());
  auto &func_cluster_info = layout_info.find(1)->second;
  EXPECT_EQ(1, func_cluster_info.clusters.size());
  EXPECT_EQ("foo", func_cluster_info.cfg->GetPrimaryName());
  // TODO(rahmanl) NodeChainBuilder must be improved so it can find {1,2,3}
  // which is optimal.
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 3, 1}));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_intra_score,
            func_cluster_info.original_intra_score);
}

TEST(CodeLayoutTest, FindOptimalLoopLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_loop.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  EXPECT_EQ(whole_program_info->cfgs().size(), 1);
  auto layout_info = CodeLayout(whole_program_info->GetHotCfgs()).OrderAll();
  EXPECT_EQ(1, layout_info.size());
  EXPECT_NE(layout_info.find(1), layout_info.end());
  auto &func_cluster_info = layout_info.find(1)->second;
  EXPECT_EQ(1, func_cluster_info.clusters.size());
  EXPECT_EQ("foo", func_cluster_info.cfg->GetPrimaryName());
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 1, 3, 4}));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_intra_score,
            func_cluster_info.original_intra_score);
}

TEST(CodeLayoutTest, FindOptimalNestedLoopLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_nested_loop.protobuf");
  ASSERT_NE(nullptr, whole_program_info);
  EXPECT_EQ(whole_program_info->cfgs().size(), 1);
  auto layout_info = CodeLayout(whole_program_info->GetHotCfgs()).OrderAll();
  EXPECT_EQ(1, layout_info.size());
  EXPECT_NE(layout_info.find(1), layout_info.end());
  auto &func_cluster_info = layout_info.find(1)->second;
  EXPECT_EQ(1, func_cluster_info.clusters.size());
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 3, 1, 4, 5, 2}));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_intra_score,
            func_cluster_info.original_intra_score);
}

TEST(CodeLayoutTest, FindOptimalMultiFunctionLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_multi_function.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  EXPECT_EQ(whole_program_info->cfgs().size(), 4);
  auto layout_info = CodeLayout(whole_program_info->GetHotCfgs()).OrderAll();
  EXPECT_EQ(3, layout_info.size());

  EXPECT_NE(layout_info.find(1), layout_info.end());
  auto &func_cluster_info_1 = layout_info.find(1)->second;
  EXPECT_EQ(1, func_cluster_info_1.clusters.size());

  EXPECT_NE(layout_info.find(4), layout_info.end());
  auto &func_cluster_info_4 = layout_info.find(4)->second;
  EXPECT_EQ(1, func_cluster_info_4.clusters.size());

  EXPECT_NE(layout_info.find(9), layout_info.end());
  auto &func_cluster_info_9 = layout_info.find(9)->second;
  EXPECT_EQ(1, func_cluster_info_9.clusters.size());

  // Check the BB clusters for every function.
  EXPECT_EQ("foo", func_cluster_info_1.cfg->GetPrimaryName());
  EXPECT_THAT(func_cluster_info_1.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 2, 1}));
  EXPECT_EQ("bar", func_cluster_info_4.cfg->GetPrimaryName());
  EXPECT_THAT(func_cluster_info_4.clusters.front().bb_indexes,
              testing::ElementsAreArray({0, 1, 3}));
  EXPECT_EQ("qux", func_cluster_info_9.cfg->GetPrimaryName());
  EXPECT_THAT(func_cluster_info_9.clusters.front().bb_indexes,
              testing::ElementsAreArray({0}));

  // Verify that the new layout improves the score for 'foo' and 'bar' and keeps
  // it equal to zero for 'qux'.
  EXPECT_GT(func_cluster_info_1.optimized_intra_score,
            func_cluster_info_1.original_intra_score);
  EXPECT_GT(func_cluster_info_4.optimized_intra_score,
            func_cluster_info_4.original_intra_score);
  EXPECT_EQ(func_cluster_info_9.optimized_intra_score, 0);
  EXPECT_EQ(func_cluster_info_9.original_intra_score, 0);

  // Check the layout index of hot clusters.
  EXPECT_EQ(0, func_cluster_info_1.clusters.front().layout_index);
  EXPECT_EQ(1, func_cluster_info_4.clusters.front().layout_index);
  EXPECT_EQ(2, func_cluster_info_9.clusters.front().layout_index);

  // Check that the layout indices of cold clusters are consistent with their
  // hot counterparts.
  EXPECT_EQ(0, func_cluster_info_1.cold_cluster_layout_index);
  EXPECT_EQ(1, func_cluster_info_4.cold_cluster_layout_index);
  EXPECT_EQ(2, func_cluster_info_9.cold_cluster_layout_index);
}
}  // namespace
