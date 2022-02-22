#include "llvm_propeller_code_layout.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_cluster_builder.h"
#include "llvm_propeller_node_chain_assembly.h"
#include "llvm_propeller_node_chain_builder.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_join.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {

// Returns the intra-procedural ext-tsp scores for the given CFGs given a
// function for getting the address of each CFG node.
// This is called by ComputeOrigLayoutScores and ComputeOptLayoutScores below.
CFGScoreMapTy CodeLayout::ComputeCfgScores(
    absl::FunctionRef<uint64_t(const CFGNode *)> get_node_addr) {
  CFGScoreMapTy score_map;
  for (const ControlFlowGraph *cfg : cfgs_) {
    uint64_t intra_score = 0;
    for (const auto &edge : cfg->intra_edges()) {
      if (edge->weight() == 0) continue;
      // Compute the distance between the end of src and beginning of sink.
      int64_t distance = static_cast<int64_t>(get_node_addr(edge->sink())) -
                         get_node_addr(edge->src()) - edge->src()->size();
      intra_score += code_layout_scorer_.GetEdgeScore(*edge, distance);
    }
    uint64_t inter_out_score = 0;
    for (const auto &edge : cfg->inter_edges()) {
      if (edge->weight() == 0 || edge->IsReturn()) continue;
      int64_t distance = static_cast<int64_t>(get_node_addr(edge->sink())) -
                         get_node_addr(edge->src()) - edge->src()->size();
      inter_out_score += code_layout_scorer_.GetEdgeScore(*edge, distance);
    }
    score_map.emplace(cfg, CFGScore({intra_score, inter_out_score}));
  }
  return score_map;
}

// Returns the intra-procedural ext-tsp scores for the given CFGs under the
// original layout.
CFGScoreMapTy CodeLayout::ComputeOrigLayoutScores() {
  return ComputeCfgScores([](const CFGNode *n) { return n->addr(); });
}

// Returns the intra-procedural ext-tsp scores for the given CFGs under the new
// layout, which is described by the 'clusters' parameter.
CFGScoreMapTy CodeLayout::ComputeOptLayoutScores(
    std::vector<std::unique_ptr<ChainCluster>> &clusters) {
  // First compute the address of each basic block under the given layout.
  uint64_t layout_addr = 0;
  absl::flat_hash_map<const CFGNode *, uint64_t> layout_address_map;
  for (auto &cluster : clusters) {
    cluster->VisitEachNodeRef([&](CFGNode &node) {
      layout_address_map.emplace(&node, layout_addr);
      layout_addr += node.size();
    });
  }

  return ComputeCfgScores([&layout_address_map](const CFGNode *n) {
    return layout_address_map.at(n);
  });
}

std::vector<FunctionClusterInfo> CodeLayout::OrderAll() {
  // Build optimal node chains for eac CFG.
  // TODO(rahmanl) Call NodeChainBuilder(cfgs_).BuildChains() for interp
  std::vector<std::unique_ptr<NodeChain>> built_chains;
  for (auto *cfg : cfgs_) {
    auto chains =
        NodeChainBuilder(code_layout_scorer_, *cfg, stats_).BuildChains();
    std::move(chains.begin(), chains.end(), std::back_inserter(built_chains));
  }

  LOG(INFO) << stats_.DebugString();

  // Further cluster the constructed chains to get the global order of all
  // nodes.
  auto clusters = ChainClusterBuilder(std::move(built_chains)).BuildClusters();

  // Order clusters consistent with the original ordering.
  // TODO(rahmanl): Order clusters in decreasing order of their exec density.
  absl::c_sort(clusters,
               [](auto &lhs, auto &rhs) { return lhs->id() < rhs->id(); });

  CFGScoreMapTy orig_score_map = ComputeOrigLayoutScores();
  CFGScoreMapTy opt_score_map = ComputeOptLayoutScores(clusters);

  // Mapping from the function ordinal to the layout cluster info.
  absl::flat_hash_map<uint64_t, FunctionClusterInfo> function_cluster_info_map;

  ControlFlowGraph *cfg = nullptr;
  unsigned layout_index = 0;

  // Cold clusters are laid out consistently with how hot clusters appear in the
  // layout. For two functions foo and bar, foo's cold cluster is placed before
  // bar's cold cluster iff (any) hot cluster of foo appears before (all) hot
  // clusters of bar.
  unsigned cold_cluster_layout_index = 0;

  auto func_cluster_info_it = function_cluster_info_map.end();

  // Iterate over all CFG nodes in order and add them to the cluster layout
  // information.
  for (auto &cluster : clusters) {
    cluster->VisitEachNodeRef([&](auto &n) {
      if (cfg != n.cfg() || n.is_entry()) {
        // Switch to the right cluster layout info when the function changes or
        // Or when an entry basic block is reached.
        cfg = n.cfg();
        uint64_t func_symbol_ordinal = cfg->GetEntryNode()->symbol_ordinal();
        bool inserted = false;
        std::tie(func_cluster_info_it, inserted) =
            function_cluster_info_map.insert(
                {func_symbol_ordinal,
                 {.cfg = cfg,
                  // We populate the clusters vector later.
                  .clusters = {},
                  .original_score = orig_score_map.at(cfg),
                  .optimized_score = opt_score_map.at(cfg),
                  .cold_cluster_layout_index = cold_cluster_layout_index}});
        if (inserted) ++cold_cluster_layout_index;
        // Start a new cluster and increment the global layout index.
        func_cluster_info_it->second.clusters.emplace_back(layout_index++);
      }
      func_cluster_info_it->second.clusters.back().bb_indexes.push_back(
          n.bb_index());
    });
  }
  std::vector<FunctionClusterInfo> all_function_cluster_info;
  all_function_cluster_info.reserve(function_cluster_info_map.size());
  for (auto &[unused, func_cluster_info] : function_cluster_info_map) {
    all_function_cluster_info.push_back(std::move(func_cluster_info));
  }

  // For each function cluster info, sort the BB clusters in increasing order of
  // their first basic block index to make sure they appear in a fixed order in
  // the cluster file which is independent from the global cluster ordering.
  // TODO(rahmanl): Test the cluster order once we have interproc-reordering.
  for (auto &func_cluster_info : all_function_cluster_info) {
    std::sort(func_cluster_info.clusters.begin(),
              func_cluster_info.clusters.end(),
              [](const FunctionClusterInfo::BBCluster &a,
                 const FunctionClusterInfo::BBCluster &b) {
                return a.bb_indexes.front() < b.bb_indexes.front();
              });
  }
  // For determinism, order the function cluster info elements in
  // lexicographical order of their (primary) function name.
  std::sort(all_function_cluster_info.begin(), all_function_cluster_info.end(),
            [](const FunctionClusterInfo &a, const FunctionClusterInfo &b) {
              return a.cfg->GetPrimaryName().compare(b.cfg->GetPrimaryName()) <
                     0;
            });

  return all_function_cluster_info;
}
}  // namespace devtools_crosstool_autofdo
