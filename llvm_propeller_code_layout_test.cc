#include "llvm_propeller_code_layout.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_cfg.pb.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_mock_whole_program_info.h"
#include "llvm_propeller_node_chain.h"
#include "llvm_propeller_node_chain_assembly.h"
#include "llvm_propeller_node_chain_builder.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_whole_program_info.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/string_view.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define ASSERT_OK(exp) ASSERT_TRUE((exp).ok())
#define EXPECT_OK(exp) EXPECT_TRUE((exp).ok())
#define ASSERT_OK_AND_ASSIGN(a, b) auto _vab = b; ASSERT_TRUE(_vab.ok()); a = std::move(_vab.value());

namespace {

using ::devtools_crosstool_autofdo::CfgCreationMode;
using ::devtools_crosstool_autofdo::CFGEdge;
using ::devtools_crosstool_autofdo::CFGNode;
using ::devtools_crosstool_autofdo::CFGNodeBundle;
using ::devtools_crosstool_autofdo::CodeLayout;
using ::devtools_crosstool_autofdo::CodeLayoutStats;
using ::devtools_crosstool_autofdo::ControlFlowGraph;
using ::devtools_crosstool_autofdo::FunctionClusterInfo;
using ::devtools_crosstool_autofdo::MergeOrder;
using ::devtools_crosstool_autofdo::MockPropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::NodeChain;
using ::devtools_crosstool_autofdo::NodeChainAssembly;
using ::devtools_crosstool_autofdo::NodeChainBuilder;
using ::devtools_crosstool_autofdo::NodeChainSlice;
using ::devtools_crosstool_autofdo::PropellerCodeLayoutParameters;
using ::devtools_crosstool_autofdo::PropellerCodeLayoutScorer;
using ::devtools_crosstool_autofdo::PropellerOptions;
using ::devtools_crosstool_autofdo::PropellerOptionsBuilder;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

MATCHER_P4(CfgEdgeIs, source_id, sink_id, weight, kind, "") {
  return arg->src()->symbol_ordinal() == source_id &&
         arg->sink()->symbol_ordinal() == sink_id &&
         arg->weight() == weight && arg->kind() == kind;
}

MATCHER_P(ChainIdIs, chain_id, "") {
  return arg->id() == chain_id;
}

static std::unique_ptr<MockPropellerWholeProgramInfo> GetTestWholeProgramInfo(
    const std::string &testdata_path) {
  const PropellerOptions options(
      PropellerOptionsBuilder()
          .AddPerfNames(FLAGS_test_srcdir + testdata_path)
          .SetCodeLayoutParamsChainSplit(true));
  auto whole_program_info = std::make_unique<
      devtools_crosstool_autofdo::MockPropellerWholeProgramInfo>(options);
  if (!whole_program_info->CreateCfgs(CfgCreationMode::kAllFunctions).ok()) {
    return nullptr;
  }
  return whole_program_info;
}

// Helper method to capture the ordinals of a chain's nodes in a vector.
static std::vector<uint64_t> GetOrderedNodeIds(
    const std::unique_ptr<NodeChain> &chain) {
  std::vector<uint64_t> node_ids;
  chain->VisitEachNodeRef(
      [&node_ids](auto &n) { node_ids.push_back(n.symbol_ordinal()); });
  return node_ids;
}

static std::vector<uint64_t> GetOrderedNodeIds(
    const NodeChainAssembly &assembly) {
  std::vector<uint64_t> node_ids;
  assembly.VisitEachNodeBundleInAssemblyOrder(
      [&node_ids](const CFGNodeBundle &bundle) {
        for (const CFGNode *node : bundle.nodes_)
          node_ids.push_back(node->symbol_ordinal());
      });
  return node_ids;
}

// Captures the nodes of a cfg keyed by their symbol ordinal.
static absl::flat_hash_map<uint64_t, CFGNode *> GetCfgNodes(
    const ControlFlowGraph &cfg) {
  absl::flat_hash_map<uint64_t, CFGNode *> ordinal_to_node_map;
  for (const std::unique_ptr<CFGNode> &node_ptr : cfg.nodes())
    ordinal_to_node_map.emplace(node_ptr->symbol_ordinal(), node_ptr.get());
  return ordinal_to_node_map;
}

// Creates one chain containing the given nodes.
static NodeChain CreateNodeChain(absl::Span<CFGNode *const> nodes) {
  CHECK(!nodes.empty());
  NodeChain chain(nodes.front());
  for (int i = 1; i < nodes.size(); ++i) {
    NodeChain other_chain(nodes[i]);
    chain.MergeWith(other_chain);
  }
  return chain;
}

// Returns a NodeChainBuilder for a single CFG, initializes the chains and
// chain edge. `stats` must outlive the returned NodeChainBuilder.
static NodeChainBuilder InitializeNodeChainBuilderForCfg(
    const MockPropellerWholeProgramInfo &whole_program_info,
    absl::string_view cfg_name, CodeLayoutStats &stats) {
  const PropellerCodeLayoutScorer scorer(
      whole_program_info.options().code_layout_params());
  NodeChainBuilder chain_builder(
      scorer, *whole_program_info.cfgs().at(cfg_name), stats);
  chain_builder.InitNodeChains();
  chain_builder.InitChainEdges();
  return chain_builder;
}

// Given a NodeChainAssembly and a CFG, returns the slice indices of all the
// CFG nodes in that assembly. The return value is a map keyed by ordinals of
// the CFG nodes mapped to their slice index in the assembly (or std::nullopt)
// if they don't occur in the assembly.
static absl::flat_hash_map<uint64_t, std::optional<int>> GetSliceIndices(
    const NodeChainAssembly &assembly, const ControlFlowGraph &cfg) {
  absl::flat_hash_map<uint64_t, std::optional<int>> slice_index_map;
  for (const std::unique_ptr<CFGNode> &node : cfg.nodes()) {
    slice_index_map.emplace(node->symbol_ordinal(),
                            assembly.FindSliceIndex(node.get()));
  }
  return slice_index_map;
}

// Test the proper construction of NodeChainSlice
TEST(NodeChainSliceTest, TestCreateNodeChainSlice) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_three_branches.protobuf");
  const ControlFlowGraph &foo_cfg = *whole_program_info->cfgs().at("foo");
  const PropellerCodeLayoutScorer scorer(
      whole_program_info->options().code_layout_params());
  absl::flat_hash_map<uint64_t, CFGNode *> foo_nodes = GetCfgNodes(foo_cfg);
  NodeChain chain = CreateNodeChain(
      {foo_nodes.at(1), foo_nodes.at(2), foo_nodes.at(3)});
  NodeChainSlice chain_slice1(chain, 0, 2);
  EXPECT_EQ(chain_slice1.begin_offset(), 0);
  EXPECT_EQ(chain_slice1.end_offset(),
            foo_nodes.at(1)->size() + foo_nodes.at(2)->size());
  NodeChainSlice chain_slice2(chain, 1, 3);
  EXPECT_EQ(chain_slice2.begin_offset(), foo_nodes.at(1)->size());
  EXPECT_EQ(chain_slice2.end_offset(), chain.size_);
  EXPECT_EQ(chain_slice2.size(),
            foo_nodes.at(2)->size() + foo_nodes.at(3)->size());
  EXPECT_EQ(chain_slice2.end_pos(), chain.node_bundles_.end());
  EXPECT_EQ(chain_slice2.begin_pos(), chain.node_bundles_.begin()+1);
  EXPECT_DEATH(NodeChainSlice(chain, 2, 1), "begin <= end");
  EXPECT_DEATH(NodeChainSlice(chain, 4, 5),
               "begin <= chain.node_bundles_.size()");
}

// Check that when multiplying the code layout parameters results in integer
// overflow, constructing the scorer crashes.
TEST(CodeLayoutScorerTest, TestOverflow) {
  PropellerCodeLayoutParameters params;
  params.set_fallthrough_weight(1 << 2);
  params.set_forward_jump_weight(1);
  params.set_backward_jump_weight(1);
  params.set_forward_jump_distance(1 << 10);
  params.set_backward_jump_distance(1 << 20);
  ASSERT_DEATH(PropellerCodeLayoutScorer scorer(params), "Integer overflow");
  params.set_fallthrough_weight(1);
  params.set_backward_jump_weight(1);
  params.set_forward_jump_weight(1 << 10);
  params.set_forward_jump_distance(0);
  params.set_backward_jump_distance(1 << 22);
  ASSERT_DEATH(PropellerCodeLayoutScorer scorer(params), "Integer overflow");
  params.set_fallthrough_weight(1);
  params.set_backward_jump_weight(1 << 10);
  params.set_forward_jump_weight(1);
  params.set_forward_jump_distance(1 << 22);
  params.set_backward_jump_distance(0);
  ASSERT_DEATH(PropellerCodeLayoutScorer scorer(params), "Integer overflow");
}

TEST(CodeLayoutScorerTest, GetEdgeScore) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_multi_function.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  const ControlFlowGraph &foo_cfg = *whole_program_info->cfgs().at("foo");
  const ControlFlowGraph &bar_cfg = *whole_program_info->cfgs().at("bar");

  // Build a layout scorer with specific parameters.
  PropellerCodeLayoutParameters params;
  params.set_fallthrough_weight(10);
  params.set_forward_jump_weight(2);
  params.set_backward_jump_weight(1);
  params.set_forward_jump_distance(200);
  params.set_backward_jump_distance(100);
  PropellerCodeLayoutScorer scorer(params);

  ASSERT_THAT(bar_cfg.inter_edges(), SizeIs(1));
  {
    const auto &call_edge = bar_cfg.inter_edges().front();
    ASSERT_TRUE(call_edge->IsCall());
    ASSERT_NE(call_edge->weight(), 0);
    ASSERT_NE(call_edge->src()->size(), 0);
    // Score with negative src-to-sink distance (backward call).
    // Check that for calls, half of src size is always added to the distance.
    EXPECT_EQ(scorer.GetEdgeScore(*call_edge, -10),
              call_edge->weight() * 1 * 200 *
                  (100 - 10 + call_edge->src()->size() / 2));
    // Score with zero src-to-sink distance (forward call).
    EXPECT_EQ(
        scorer.GetEdgeScore(*call_edge, 0),
        call_edge->weight() * 2 * 100 * (200 - call_edge->src()->size() / 2));
    // Score with positive src-to-sink distance (forward call).
    EXPECT_EQ(scorer.GetEdgeScore(*call_edge, 20),
              call_edge->weight() * 2 * 100 *
                  (200 - 20 - call_edge->src()->size() / 2));
    // Score must be zero when beyond the src-to-sink distance exceeds the
    // distance parameters.
    EXPECT_EQ(scorer.GetEdgeScore(*call_edge, 250), 0);
    EXPECT_EQ(scorer.GetEdgeScore(*call_edge, -150), 0);
  }

  ASSERT_THAT(foo_cfg.inter_edges(), SizeIs(2));
  for (const std::unique_ptr<CFGEdge> &ret_edge : foo_cfg.inter_edges()) {
    ASSERT_TRUE(ret_edge->IsReturn());
    ASSERT_NE(ret_edge->weight(), 0);
    ASSERT_NE(ret_edge->sink()->size(), 0);
    // Score with negative src-to-sink distance (backward return).
    // Check that for returns, half of sink size is always added to the
    // distance.
    EXPECT_EQ(scorer.GetEdgeScore(*ret_edge, -10),
              ret_edge->weight() * 1 * 200 *
                  (100 - 10 + ret_edge->sink()->size() / 2));
    // Score with zero src-to-sink distance (forward return).
    EXPECT_EQ(
        scorer.GetEdgeScore(*ret_edge, 0),
        ret_edge->weight() * 2 * 100 * (200 - ret_edge->sink()->size() / 2));
    // Score with positive src-to-sink distance (forward return).
    EXPECT_EQ(scorer.GetEdgeScore(*ret_edge, 20),
              ret_edge->weight() * 2 * 100 *
                  (200 - 20 - ret_edge->sink()->size() / 2));
    EXPECT_EQ(scorer.GetEdgeScore(*ret_edge, 250), 0);
    EXPECT_EQ(scorer.GetEdgeScore(*ret_edge, -150), 0);
  }

  for (const std::unique_ptr<CFGEdge> &edge : foo_cfg.intra_edges()) {
    ASSERT_EQ(edge->kind(),
              devtools_crosstool_autofdo::CFGEdge::Kind::kBranchOrFallthough);
    ASSERT_NE(edge->weight(), 0);
    // Fallthrough score.
    EXPECT_EQ(scorer.GetEdgeScore(*edge, 0), edge->weight() * 10 * 100 * 200);
    // Backward edge (within distance threshold) score.
    EXPECT_EQ(scorer.GetEdgeScore(*edge, -40),
              edge->weight() * 1 * 200 * (100 - 40));
    // Forward edge (within distance threshold) score.
    EXPECT_EQ(scorer.GetEdgeScore(*edge, 80),
              edge->weight() * 2 * 100 * (200 - 80));
    // Forward and backward edge beyond the distance thresholds (zero score).
    EXPECT_EQ(scorer.GetEdgeScore(*edge, 201), 0);
    EXPECT_EQ(scorer.GetEdgeScore(*edge, -101), 0);
  }
}

// Check that MergeChain(NodeChain&, NodeChain&) properly updates the chain
// edges by calling MergeChainEdges.
TEST(ChainBuilderTest, MergeChainsUpdatesChainEdges) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder =
      InitializeNodeChainBuilderForCfg(*whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();

  EXPECT_THAT(
      chains.at(1)->out_edges_,
      ElementsAre(Pair(chains.at(2).get(),
                       ElementsAre(CfgEdgeIs(
                           1, 2, 110, CFGEdge::Kind::kBranchOrFallthough))),
                  Pair(chains.at(3).get(),
                       ElementsAre(CfgEdgeIs(
                           1, 3, 150, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(1)->in_edges_, IsEmpty());
  EXPECT_THAT(
      chains.at(2)->out_edges_,
      ElementsAre(Pair(chains.at(3).get(),
                       ElementsAre(CfgEdgeIs(
                           2, 3, 100, CFGEdge::Kind::kBranchOrFallthough))),
                  Pair(chains.at(4).get(),
                       ElementsAre(CfgEdgeIs(
                           2, 4, 10, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(2)->in_edges_, UnorderedElementsAre(ChainIdIs(1)));
  EXPECT_THAT(
      chains.at(3)->out_edges_,
      ElementsAre(Pair(chains.at(5).get(),
                       ElementsAre(CfgEdgeIs(
                           3, 5, 250, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(3)->in_edges_,
              UnorderedElementsAre(ChainIdIs(1), ChainIdIs(2)));
  EXPECT_THAT(
      chains.at(4)->out_edges_,
      ElementsAre(Pair(chains.at(5).get(),
                       ElementsAre(CfgEdgeIs(
                           4, 5, 10, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(4)->in_edges_, UnorderedElementsAre(ChainIdIs(2)));
  EXPECT_THAT(chains.at(5)->out_edges_, IsEmpty());
  EXPECT_THAT(chains.at(5)->in_edges_,
              UnorderedElementsAre(ChainIdIs(3), ChainIdIs(4)));

  chain_builder.MergeChains(*chains.at(2), *chains.at(4));

  EXPECT_THAT(GetOrderedNodeIds(chains.at(2)), ElementsAre(2, 4));

  EXPECT_THAT(
      chains.at(1)->out_edges_,
      ElementsAre(Pair(chains.at(2).get(),
                       ElementsAre(CfgEdgeIs(
                           1, 2, 110, CFGEdge::Kind::kBranchOrFallthough))),
                  Pair(chains.at(3).get(),
                       ElementsAre(CfgEdgeIs(
                           1, 3, 150, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(1)->in_edges_, IsEmpty());
  EXPECT_THAT(
      chains.at(2)->out_edges_,
      ElementsAre(Pair(chains.at(2).get(),
                       ElementsAre(CfgEdgeIs(
                           2, 4, 10, CFGEdge::Kind::kBranchOrFallthough))),
                  Pair(chains.at(3).get(),
                       ElementsAre(CfgEdgeIs(
                           2, 3, 100, CFGEdge::Kind::kBranchOrFallthough))),
                  Pair(chains.at(5).get(),
                       ElementsAre(CfgEdgeIs(
                           4, 5, 10, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(2)->in_edges_,
              UnorderedElementsAre(ChainIdIs(1), ChainIdIs(2)));
  EXPECT_THAT(
      chains.at(3)->out_edges_,
      ElementsAre(Pair(chains.at(5).get(),
                       ElementsAre(CfgEdgeIs(
                           3, 5, 250, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(3)->in_edges_,
              UnorderedElementsAre(ChainIdIs(1), ChainIdIs(2)));
  EXPECT_THAT(chains.at(5)->out_edges_, IsEmpty());
  EXPECT_THAT(chains.at(5)->in_edges_,
              UnorderedElementsAre(ChainIdIs(2), ChainIdIs(3)));
}

// Check that MergeChain(NodeChainAssembly) properly updates the chain
// edges by calling MergeChainEdges.
TEST(ChainBuilderTest, MergeChainsWithAssemblyUpdatesChainEdges) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder =
      InitializeNodeChainBuilderForCfg(*whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();
  chain_builder.MergeChains(*chains.at(2), *chains.at(4));
  absl::StatusOr<NodeChainAssembly> assembly =
      NodeChainAssembly::BuildNodeChainAssembly(
          chain_builder.code_layout_scorer(), *chains.at(2), *chains.at(3),
          MergeOrder::kSU, {});
  ASSERT_OK(assembly);
  chain_builder.MergeChains(*assembly);
  EXPECT_THAT(GetOrderedNodeIds(chains.at(2)), ElementsAre(2, 4, 3));

  EXPECT_THAT(
      chains.at(1)->out_edges_,
      ElementsAre(
          Pair(chains.at(2).get(),
               ElementsAre(
                   CfgEdgeIs(1, 2, 110, CFGEdge::Kind::kBranchOrFallthough),
                   CfgEdgeIs(1, 3, 150, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(1)->in_edges_, IsEmpty());
  EXPECT_THAT(
      chains.at(2)->out_edges_,
      ElementsAre(
          Pair(chains.at(2).get(),
               ElementsAre(
                   CfgEdgeIs(2, 4, 10, CFGEdge::Kind::kBranchOrFallthough),
                   CfgEdgeIs(2, 3, 100, CFGEdge::Kind::kBranchOrFallthough))),
          Pair(chains.at(5).get(),
               ElementsAre(
                   CfgEdgeIs(4, 5, 10, CFGEdge::Kind::kBranchOrFallthough),
                   CfgEdgeIs(3, 5, 250, CFGEdge::Kind::kBranchOrFallthough)))));
  EXPECT_THAT(chains.at(2)->in_edges_,
              UnorderedElementsAre(ChainIdIs(1), ChainIdIs(2)));

  EXPECT_THAT(chains.at(5)->out_edges_, IsEmpty());
  EXPECT_THAT(chains.at(5)->in_edges_, UnorderedElementsAre(ChainIdIs(2)));
}

// This tests every step in NodeChainBuilder::BuildChains on a single CFG.
TEST(CodeLayoutTest, BuildChains) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_three_branches.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  ASSERT_THAT(whole_program_info->cfgs(), SizeIs(1));
  ControlFlowGraph &foo_cfg = *whole_program_info->cfgs().at("foo");
  ASSERT_THAT(foo_cfg.nodes(), SizeIs(6));
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder(
      PropellerCodeLayoutScorer(
          whole_program_info->options().code_layout_params()),
      foo_cfg, stats);

  chain_builder.InitNodeChains();

  auto &chains = chain_builder.chains();
  // Verify that there are 5 chains corresponding to the hot nodes.
  ASSERT_THAT(chains, SizeIs(5));
  // Verify that every chain has a single node.
  for (auto &chain_elem : chains) {
    EXPECT_THAT(
        GetOrderedNodeIds(chain_elem.second),
        ElementsAre(chain_elem.second->delegate_node_->symbol_ordinal()));
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
  EXPECT_THAT(chain_builder.node_chain_assemblies(), SizeIs(5));

  while (!chain_builder.node_chain_assemblies().empty()) {
    chain_builder.MergeChains(chain_builder.GetBestAssembly());
  }

  // Verify that the chain assemblies is empty now.
  EXPECT_TRUE(chain_builder.node_chain_assemblies().empty());
  // Verify the constructed chains.
  EXPECT_EQ(2, chains.size());
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)), ElementsAre(1, 2, 5));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)), ElementsAre(3, 4));

  chain_builder.CoalesceChains();

  // Verify that the two chains are coalesced together.
  EXPECT_EQ(1, chains.size());
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)), ElementsAre(1, 2, 5, 3, 4));
}

// Tests building and applying a SU NodeChainAssembly.
TEST(NodeChainAssemblyTest, ApplySUMergeOrder) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder = InitializeNodeChainBuilderForCfg(
      *whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();

  ASSERT_OK_AND_ASSIGN(NodeChainAssembly assembly,
      NodeChainAssembly::BuildNodeChainAssembly(
      chain_builder.code_layout_scorer(), *chains.at(1),
      *chains.at(3), MergeOrder::kSU, std::nullopt));

  EXPECT_EQ(assembly.score_gain(), 1044480000);
  EXPECT_THAT(
      GetSliceIndices(assembly, *chain_builder.cfgs().front()),
      UnorderedElementsAre(Pair(1, 0), Pair(3, 1), Pair(2, std::nullopt),
                           Pair(4, std::nullopt), Pair(5, std::nullopt)));
  EXPECT_THAT(GetOrderedNodeIds(assembly), ElementsAre(1, 3));
  chain_builder.MergeChains(assembly);
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)), ElementsAre(1, 3));
  EXPECT_THAT(chains,
              ElementsAre(Pair(1, _), Pair(2, _), Pair(4, _), Pair(5, _)));
}

// Tests building and applying a S1US2 NodeChainAssembly.
TEST(NodeChainAssemblyTest, ApplyS1US2MergeOrder) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder =
      InitializeNodeChainBuilderForCfg(*whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();

  chain_builder.MergeChains(*chains.at(1), *chains.at(3));
  EXPECT_THAT(chains,
              ElementsAre(Pair(1, _), Pair(2, _), Pair(4, _), Pair(5, _)));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)), ElementsAre(1, 3));

  ASSERT_OK_AND_ASSIGN(NodeChainAssembly assembly,
      NodeChainAssembly::BuildNodeChainAssembly(
      chain_builder.code_layout_scorer(), *chains.at(1),
      *chains.at(2), MergeOrder::kS1US2, 1));

  EXPECT_THAT(
      GetSliceIndices(assembly, *chain_builder.cfgs().front()),
      UnorderedElementsAre(Pair(1, 0), Pair(2, 1), Pair(3, 2),
                           Pair(4, std::nullopt), Pair(5, std::nullopt)));
  EXPECT_EQ(assembly.score_gain(), 521832000);
  EXPECT_THAT(GetOrderedNodeIds(assembly), ElementsAre(1, 2, 3));
  chain_builder.MergeChains(assembly);
  EXPECT_THAT(GetOrderedNodeIds(chains.at(1)), ElementsAre(1, 2, 3));
  EXPECT_THAT(chains, ElementsAre(Pair(1, _), Pair(4, _), Pair(5, _)));
}

// Test for building and applying a US2S1 NodeChainAssembly.
TEST(NodeChainAssemblyTest, ApplyUS2S1MergeOrder) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder =
      InitializeNodeChainBuilderForCfg(*whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();

  chain_builder.MergeChains(*chains.at(3), *chains.at(2));
  EXPECT_THAT(chains,
              ElementsAre(Pair(1, _), Pair(3, _), Pair(4, _), Pair(5, _)));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)), ElementsAre(3, 2));

  ASSERT_OK_AND_ASSIGN(NodeChainAssembly assembly,
                       NodeChainAssembly::BuildNodeChainAssembly(
                           chain_builder.code_layout_scorer(), *chains.at(3),
                           *chains.at(1), MergeOrder::kUS2S1, 1));
  EXPECT_THAT(
      GetSliceIndices(assembly, *chain_builder.cfgs().front()),
      UnorderedElementsAre(Pair(1, 0), Pair(2, 1), Pair(3, 2),
                           Pair(4, std::nullopt), Pair(5, std::nullopt)));
  EXPECT_EQ(assembly.score_gain(), 1497089600);
  EXPECT_THAT(GetOrderedNodeIds(assembly), ElementsAre(1, 2, 3));
  chain_builder.MergeChains(assembly);
  EXPECT_THAT(chains, ElementsAre(Pair(3, _), Pair(4, _), Pair(5, _)));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)), ElementsAre(1, 2, 3));
}

// Tests building and applying a S2S1U NodeChainAssembly.
TEST(NodeChainAssemblyTest, ApplyS2S1UMergeOrder) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder =
      InitializeNodeChainBuilderForCfg(*whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();

  chain_builder.MergeChains(*chains.at(3), *chains.at(2));
  EXPECT_THAT(chains,
              ElementsAre(Pair(1, _), Pair(3, _), Pair(4, _), Pair(5, _)));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)), ElementsAre(3, 2));

  ASSERT_OK_AND_ASSIGN(NodeChainAssembly assembly,
                       NodeChainAssembly::BuildNodeChainAssembly(
                           chain_builder.code_layout_scorer(), *chains.at(3),
                           *chains.at(4), MergeOrder::kS2S1U, 1));

  EXPECT_THAT(
      GetSliceIndices(assembly, *chain_builder.cfgs().front()),
      UnorderedElementsAre(Pair(1, std::nullopt), Pair(2, 0), Pair(3, 1),
                           Pair(4, 2), Pair(5, std::nullopt)));
  EXPECT_EQ(assembly.score_gain(), 696729600);
  EXPECT_THAT(GetOrderedNodeIds(assembly), ElementsAre(2, 3, 4));
  chain_builder.MergeChains(assembly);
  EXPECT_THAT(chains, ElementsAre(Pair(1, _), Pair(3, _), Pair(5, _)));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)), ElementsAre(2, 3, 4));
}

// Tests building and applying a S2US1 NodeChainAssembly.
TEST(NodeChainAssemblyTest, ApplyS2US1MergeOrder) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder =
      InitializeNodeChainBuilderForCfg(*whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();

  chain_builder.MergeChains(*chains.at(3), *chains.at(2));
  EXPECT_THAT(chains,
              ElementsAre(Pair(1, _), Pair(3, _), Pair(4, _), Pair(5, _)));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)), ElementsAre(3, 2));

  ASSERT_OK_AND_ASSIGN(NodeChainAssembly assembly,
                       NodeChainAssembly::BuildNodeChainAssembly(
                           chain_builder.code_layout_scorer(), *chains.at(3),
                           *chains.at(4), MergeOrder::kS2US1, 1));
  EXPECT_THAT(
      GetSliceIndices(assembly, *chain_builder.cfgs().front()),
      UnorderedElementsAre(Pair(1, std::nullopt), Pair(2, 0), Pair(3, 2),
                           Pair(4, 1), Pair(5, std::nullopt)));
  EXPECT_EQ(assembly.score_gain(), 69905600);
  EXPECT_THAT(GetOrderedNodeIds(assembly), ElementsAre(2, 4, 3));
  chain_builder.MergeChains(assembly);
  EXPECT_THAT(chains, ElementsAre(Pair(1, _), Pair(3, _), Pair(5, _)));
  EXPECT_THAT(GetOrderedNodeIds(chains.at(3)), ElementsAre(2, 4, 3));
}

// Test for detecting invalid assemblies built from single node chains.
TEST(NodeChainAssemblyTest, InvalidAssembliesFail1) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder =
      InitializeNodeChainBuilderForCfg(*whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();

  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(1),
                   *chains.at(1), MergeOrder::kSU).IgnoreError(),
               "Cannot construct an assembly between a chain and itself.");

  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(1),
                   *chains.at(2), MergeOrder::kSU, 0).IgnoreError(),
               "slice_pos must not be provided for kSU merge order.");
  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(1),
                   *chains.at(2), MergeOrder::kS2S1U, 0).IgnoreError(),
               "Out of bounds slice position.");
  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(2),
                   *chains.at(1), MergeOrder::kS1US2, 1).IgnoreError(),
               "Out of bounds slice position.");
  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(2),
                   *chains.at(1), MergeOrder::kUS2S1).IgnoreError(),
               "slice_pos is required for every merge order other than kSU.");

  auto v = NodeChainAssembly::BuildNodeChainAssembly(
      chain_builder.code_layout_scorer(), *chains.at(2),
      *chains.at(1), MergeOrder::kSU);
  EXPECT_TRUE(!v.ok() && absl::IsFailedPrecondition(v.status()));

  auto v2 = NodeChainAssembly::BuildNodeChainAssembly(
      chain_builder.code_layout_scorer(), *chains.at(1),
      *chains.at(4), MergeOrder::kSU);
  EXPECT_TRUE(!v2.ok() && absl::IsFailedPrecondition(v2.status()));
}

// Test for detecting invalid assemblies constructed from a 2-node chain and
// single-node chain.
TEST(NodeChainAssemblyTest, InvalidAssembliesFail2) {
  std::unique_ptr<MockPropellerWholeProgramInfo> whole_program_info =
      GetTestWholeProgramInfo(
          "/testdata/"
          "propeller_simple_conditionals_join.protobuf");
  CodeLayoutStats stats;
  NodeChainBuilder chain_builder =
      InitializeNodeChainBuilderForCfg(*whole_program_info, "foo", stats);
  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains =
      chain_builder.chains();
  chain_builder.MergeChains(*chains.at(1), *chains.at(2));

  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(1),
                   *chains.at(3), MergeOrder::kS2S1U, 0).IgnoreError(),
               "Out of bounds slice position.");

  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(1),
                   *chains.at(3), MergeOrder::kS2S1U, 2).IgnoreError(),
               "Out of bounds slice position.");

  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(1),
                   *chains.at(3), MergeOrder::kUS2S1, 0).IgnoreError(),
               "Out of bounds slice position.");

  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(1),
                   *chains.at(3), MergeOrder::kUS2S1, 2).IgnoreError(),
               "Out of bounds slice position.");

  EXPECT_DEATH(NodeChainAssembly::BuildNodeChainAssembly(
                   chain_builder.code_layout_scorer(), *chains.at(1),
                   *chains.at(1), MergeOrder::kS1US2, 0)
                   .IgnoreError(),
               "Cannot construct an assembly between a chain and itself.");

  auto v = NodeChainAssembly::BuildNodeChainAssembly(
      chain_builder.code_layout_scorer(), *chains.at(1),
      *chains.at(3), MergeOrder::kS2S1U, 1);
  EXPECT_TRUE(!v.ok() && absl::IsFailedPrecondition(v.status()));

  auto v2 = NodeChainAssembly::BuildNodeChainAssembly(
      chain_builder.code_layout_scorer(), *chains.at(1),
      *chains.at(4), MergeOrder::kS1US2, 1);
  EXPECT_TRUE(!v2.ok() && absl::IsFailedPrecondition(v2.status()));
}

TEST(CodeLayoutTest, FindOptimalFallthrough) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_conditional.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  ASSERT_THAT(whole_program_info->cfgs(), SizeIs(1));
  std::vector<FunctionClusterInfo> all_func_cluster_info =
          CodeLayout(whole_program_info->options().code_layout_params(),
                     whole_program_info->GetHotCfgs())
              .OrderAll();
  ASSERT_THAT(all_func_cluster_info, SizeIs(1));
  auto &func_cluster_info = all_func_cluster_info[0];
  EXPECT_EQ(func_cluster_info.cfg->GetPrimaryName(), "foo");
  ASSERT_THAT(func_cluster_info.clusters, SizeIs(1));
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              ElementsAre(0, 1, 3));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_score.intra_score,
            func_cluster_info.original_score.intra_score);
}

TEST(CodeLayoutTest, FindOptimalLoopLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_loop.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  ASSERT_THAT(whole_program_info->cfgs(), SizeIs(1));
  std::vector<FunctionClusterInfo> all_func_cluster_info =
          CodeLayout(whole_program_info->options().code_layout_params(),
                     whole_program_info->GetHotCfgs())
              .OrderAll();
  ASSERT_THAT(all_func_cluster_info, SizeIs(1));
  auto &func_cluster_info = all_func_cluster_info[0];
  EXPECT_EQ(func_cluster_info.cfg->GetPrimaryName(), "foo");
  ASSERT_THAT(func_cluster_info.clusters, SizeIs(1));
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              ElementsAre(0, 1, 3, 4));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_score.intra_score,
            func_cluster_info.original_score.intra_score);
}

TEST(CodeLayoutTest, FindOptimalNestedLoopLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_nested_loop.protobuf");
  ASSERT_NE(nullptr, whole_program_info);
  ASSERT_THAT(whole_program_info->cfgs(), SizeIs(1));
  std::vector<FunctionClusterInfo> all_func_cluster_info =
          CodeLayout(whole_program_info->options().code_layout_params(),
                     whole_program_info->GetHotCfgs())
              .OrderAll();
  ASSERT_THAT(all_func_cluster_info, SizeIs(1));
  auto &func_cluster_info = all_func_cluster_info[0];
  ASSERT_THAT(func_cluster_info.clusters, SizeIs(1));
  EXPECT_THAT(func_cluster_info.clusters.front().bb_indexes,
              ElementsAre(0, 3, 1, 4, 5, 2));
  // Verify that the new layout improves the score.
  EXPECT_GT(func_cluster_info.optimized_score.intra_score,
            func_cluster_info.original_score.intra_score);
}

TEST(CodeLayoutTest, FindOptimalMultiFunctionLayout) {
  auto whole_program_info = GetTestWholeProgramInfo(
      "/testdata/"
      "propeller_simple_multi_function.protobuf");
  ASSERT_NE(nullptr, whole_program_info);

  ASSERT_THAT(whole_program_info->cfgs(), SizeIs(4));
  std::vector<FunctionClusterInfo> all_func_cluster_info =
          CodeLayout(whole_program_info->options().code_layout_params(),
                     whole_program_info->GetHotCfgs())
              .OrderAll();
  ASSERT_THAT(all_func_cluster_info, SizeIs(3));

  ASSERT_THAT(all_func_cluster_info[0].clusters, SizeIs(1));
  ASSERT_THAT(all_func_cluster_info[1].clusters, SizeIs(1));
  ASSERT_THAT(all_func_cluster_info[2].clusters, SizeIs(1));

  // Check the BB clusters for every function.
  EXPECT_EQ(all_func_cluster_info[0].cfg->GetPrimaryName(), "bar");
  EXPECT_THAT(all_func_cluster_info[0].clusters.front().bb_indexes,
              ElementsAre(0, 1, 3));
  EXPECT_EQ(all_func_cluster_info[1].cfg->GetPrimaryName(), "foo");
  EXPECT_THAT(all_func_cluster_info[1].clusters.front().bb_indexes,
              ElementsAre(0, 2, 1));
  EXPECT_EQ(all_func_cluster_info[2].cfg->GetPrimaryName(), "qux");
  EXPECT_THAT(all_func_cluster_info[2].clusters.front().bb_indexes,
              ElementsAre(0));

  // Verify that the new layout improves the score for 'foo' and 'bar' and keeps
  // it equal to zero for 'qux'.
  for (int i : {0, 1})
    EXPECT_GT(all_func_cluster_info[i].optimized_score.intra_score,
              all_func_cluster_info[i].original_score.intra_score);

  EXPECT_EQ(all_func_cluster_info[2].optimized_score.intra_score, 0);
  EXPECT_EQ(all_func_cluster_info[2].original_score.intra_score, 0);
  // TODO(rahmanl): Check for improvement in inter_out_score once we have
  // function reordering.

  // Check the layout index of hot clusters.
  EXPECT_EQ(all_func_cluster_info[0].clusters.front().layout_index, 1);
  EXPECT_EQ(all_func_cluster_info[1].clusters.front().layout_index, 0);
  EXPECT_EQ(all_func_cluster_info[2].clusters.front().layout_index, 2);

  // Check that the layout indices of cold clusters are consistent with their
  // hot counterparts.
  EXPECT_EQ(all_func_cluster_info[0].cold_cluster_layout_index, 1);
  EXPECT_EQ(all_func_cluster_info[1].cold_cluster_layout_index, 0);
  EXPECT_EQ(all_func_cluster_info[2].cold_cluster_layout_index, 2);
}

}  // namespace
