#include "llvm_propeller_code_layout.h"

#include <iterator>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_cluster_builder.h"
#include "llvm_propeller_function_cluster_info.h"
#include "llvm_propeller_node_chain_builder.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_program_cfg.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/types/span.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {

absl::btree_map<llvm::StringRef, std::vector<FunctionClusterInfo>>
GenerateLayoutBySection(const ProgramCfg &program_cfg,
                        const PropellerCodeLayoutParameters &code_layout_params,
                        PropellerStats::CodeLayoutStats &code_layout_stats) {
  absl::btree_map<llvm::StringRef, std::vector<FunctionClusterInfo>>
      cluster_info_by_section_name;
  absl::flat_hash_map<llvm::StringRef, std::vector<const ControlFlowGraph *>>
      cfgs_by_section_name = program_cfg.GetCfgsBySectionName();
  for (const auto &[section_name, cfgs] : cfgs_by_section_name) {
    CodeLayout code_layout(code_layout_params, cfgs);
    cluster_info_by_section_name.emplace(section_name, code_layout.OrderAll());
    code_layout_stats += code_layout.stats();
  }
  return cluster_info_by_section_name;
}

// Returns the intra-procedural ext-tsp scores for the given CFGs given a
// function for getting the address of each CFG node.
// This is called by ComputeOrigLayoutScores and ComputeOptLayoutScores below.
absl::flat_hash_map<int, CFGScore> CodeLayout::ComputeCfgScores(
    absl::FunctionRef<uint64_t(const CFGNode *)> get_node_addr) {
  absl::flat_hash_map<int, CFGScore> score_map;
  for (const ControlFlowGraph *cfg : cfgs_) {
    double intra_score = 0;
    for (const auto &edge : cfg->intra_edges()) {
      if (edge->weight() == 0 || edge->IsReturn()) continue;
      // Compute the distance between the end of src and beginning of sink.
      int64_t distance = static_cast<int64_t>(get_node_addr(edge->sink())) -
                         get_node_addr(edge->src()) - edge->src()->size();
      intra_score += code_layout_scorer_.GetEdgeScore(*edge, distance);
    }
    double inter_out_score = 0;
    if (cfgs_.size() > 1) {
      for (const auto &edge : cfg->inter_edges()) {
        if (edge->weight() == 0 || edge->IsReturn() || edge->inter_section()) {
          continue;
        }
        int64_t distance = static_cast<int64_t>(get_node_addr(edge->sink())) -
                           get_node_addr(edge->src()) - edge->src()->size();
        inter_out_score += code_layout_scorer_.GetEdgeScore(*edge, distance);
      }
    }
    score_map.emplace(cfg->function_index(),
                      CFGScore({intra_score, inter_out_score}));
  }
  return score_map;
}

// Returns the intra-procedural ext-tsp scores for the given CFGs under the
// original layout.
absl::flat_hash_map<int, CFGScore> CodeLayout::ComputeOrigLayoutScores() {
  return ComputeCfgScores([](const CFGNode *n) { return n->addr(); });
}

// Returns the intra-procedural ext-tsp scores for the given CFGs under the new
// layout, which is described by the 'clusters' parameter.
absl::flat_hash_map<int, CFGScore> CodeLayout::ComputeOptLayoutScores(
    absl::Span<const std::unique_ptr<const ChainCluster>> clusters) {
  // First compute the address of each basic block under the given layout.
  uint64_t layout_addr = 0;
  absl::flat_hash_map<const CFGNode *, uint64_t> layout_address_map;
  for (auto &cluster : clusters) {
    cluster->VisitEachNodeRef([&](const CFGNode &node) {
      layout_address_map.emplace(&node, layout_addr);
      layout_addr += node.size();
    });
  }

  return ComputeCfgScores([&layout_address_map](const CFGNode *n) {
    return layout_address_map.at(n);
  });
}

std::vector<FunctionClusterInfo> CodeLayout::OrderAll() {
  // Build optimal node chains for each CFG.
  std::vector<std::unique_ptr<const NodeChain>> built_chains;
  if (code_layout_scorer_.code_layout_params().inter_function_reordering()) {
    absl::c_move(NodeChainBuilder::CreateNodeChainBuilder<
                     NodeChainAssemblyBalancedTreeQueue>(
                     code_layout_scorer_, cfgs_, initial_chains_, stats_)
                     .BuildChains(),
                 std::back_inserter(built_chains));
  } else {
    for (auto *cfg : cfgs_) {
      absl::c_move(NodeChainBuilder::CreateNodeChainBuilder<
                       NodeChainAssemblyIterativeQueue>(
                       code_layout_scorer_, {cfg}, initial_chains_, stats_)
                       .BuildChains(),
                   std::back_inserter(built_chains));
    }
  }

  // Further cluster the constructed chains to get the global order of all
  // nodes.
  const std::vector<std::unique_ptr<const ChainCluster>> clusters =
      ChainClusterBuilder(code_layout_scorer_.code_layout_params(),
                          std::move(built_chains))
          .BuildClusters();

  absl::flat_hash_map<int, CFGScore> orig_score_map = ComputeOrigLayoutScores();
  absl::flat_hash_map<int, CFGScore> opt_score_map =
      ComputeOptLayoutScores(clusters);

  // Mapping from the function ordinal to the layout cluster info.
  absl::flat_hash_map<int, FunctionClusterInfo> function_cluster_info_map;

  int function_index = -1;
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
    cluster->VisitEachNodeRef([&](const CFGNode &node) {
      if (function_index != node.function_index() || node.is_entry()) {
        // Switch to the right cluster layout info when the function changes or
        // Or when an entry basic block is reached.
        function_index = node.function_index();
        bool inserted = false;
        std::tie(func_cluster_info_it, inserted) =
            function_cluster_info_map.insert(
                {function_index,
                 {.function_index = function_index,
                  // We populate the clusters vector later.
                  .clusters = {},
                  .original_score = orig_score_map.at(function_index),
                  .optimized_score = opt_score_map.at(function_index),
                  .cold_cluster_layout_index = cold_cluster_layout_index}});
        if (inserted) ++cold_cluster_layout_index;
        // Start a new cluster and increment the global layout index.
        func_cluster_info_it->second.clusters.emplace_back(layout_index++);
      }
      func_cluster_info_it->second.clusters.back().full_bb_ids.push_back(
          node.full_intra_cfg_id());
    });
  }
  std::vector<FunctionClusterInfo> all_function_cluster_info;
  all_function_cluster_info.reserve(function_cluster_info_map.size());
  for (auto &[unused, func_cluster_info] : function_cluster_info_map) {
    stats_.original_intra_score += func_cluster_info.original_score.intra_score;
    stats_.optimized_intra_score +=
        func_cluster_info.optimized_score.intra_score;
    stats_.original_inter_score +=
        func_cluster_info.original_score.inter_out_score;
    stats_.optimized_inter_score +=
        func_cluster_info.optimized_score.inter_out_score;
    all_function_cluster_info.push_back(std::move(func_cluster_info));
  }

  // For each function cluster info, sort the BB clusters in increasing order of
  // their first basic block id to make sure they appear in a fixed order in
  // the cluster file which is independent from the global cluster ordering.
  // TODO(rahmanl): Test the cluster order once we have interproc-reordering.
  for (auto &func_cluster_info : all_function_cluster_info) {
    absl::c_sort(func_cluster_info.clusters,
                 [](const FunctionClusterInfo::BBCluster &a,
                    const FunctionClusterInfo::BBCluster &b) {
                   return a.full_bb_ids.front().bb_id <
                          b.full_bb_ids.front().bb_id;
                 });
  }
  // For determinism, order the function cluster info elements in increasing
  // order of their function index (consistent with the original function
  // ordering).
  absl::c_sort(all_function_cluster_info,
               [](const FunctionClusterInfo &a, const FunctionClusterInfo &b) {
                 return a.function_index < b.function_index;
               });

  return all_function_cluster_info;
}
}  // namespace devtools_crosstool_autofdo
