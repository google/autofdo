#include "llvm_propeller_code_layout.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_cluster_builder.h"
#include "llvm_propeller_node_chain_builder.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"

namespace devtools_crosstool_autofdo {

namespace {
using CFGScoreMapTy = absl::flat_hash_map<const ControlFlowGraph *, uint64_t>;

// Returns the intra-procedural ext-tsp scores for the given CFGs given a
// function for getting the address of each CFG node.
// This is called by ComputeOrigLayoutScores and ComputeOptLayoutScores below.
template <class GetNodeAddrFunc>
CFGScoreMapTy ComputeCfgScores(const std::vector<ControlFlowGraph *> &cfgs,
                               GetNodeAddrFunc &&get_addr) {
  CFGScoreMapTy score_map;
  for (const ControlFlowGraph *cfg : cfgs) {
    uint64_t intra_score = 0;
    for (const auto &edge : cfg->intra_edges_) {
      if (edge->weight_ == 0) continue;
      // Compute the distance between the end of src and beginning of sink.
      int64_t distance = static_cast<int64_t>(get_addr(edge->sink_)) -
                         get_addr(edge->src_) - edge->src_->size_;
      intra_score += GetEdgeScore(edge.get(), distance);
    }
    score_map.emplace(cfg, intra_score);
  }
  return score_map;
}

// Returns the intra-procedural ext-tsp scores for the given CFGs under the
// original layout.
CFGScoreMapTy ComputeOrigLayoutScores(
    const std::vector<ControlFlowGraph *> &cfgs) {
  return ComputeCfgScores(cfgs, [](const CFGNode *n) { return n->addr_; });
}

// Returns the intra-procedural ext-tsp scores for the given CFGs under the new
// layout, which is described by the 'clusters' parameter.
CFGScoreMapTy ComputeOptLayoutScores(
    const std::vector<ControlFlowGraph *> &cfgs,
    std::vector<std::unique_ptr<ChainCluster>> &clusters) {
  // First compute the address of each basic block under the given layout.
  uint64_t layout_addr = 0;
  absl::flat_hash_map<const CFGNode *, uint64_t> layout_address_map;
  for (auto &cluster : clusters) {
    cluster->VisitEachNodeRef([&](CFGNode &node) {
      layout_address_map.emplace(&node, layout_addr);
      layout_addr += node.size_;
    });
  }

  return ComputeCfgScores(cfgs, [&layout_address_map](const CFGNode *n) {
    return layout_address_map.at(n);
  });
}
}  // namespace

CodeLayoutResult CodeLayout::OrderAll() {
  // Build optimal node chains for eac CFG.
  // TODO(rahmanl) Call NodeChainBuilder(cfgs_).BuildChains() for interp
  std::vector<std::unique_ptr<NodeChain>> built_chains;
  for (auto *cfg : cfgs_) {
    auto chains = NodeChainBuilder(cfg).BuildChains();
    std::move(chains.begin(), chains.end(), std::back_inserter(built_chains));
  }
  // Further cluster the constructed chains to get the global order of all
  // nodes.
  auto clusters = ChainClusterBuilder(std::move(built_chains)).BuildClusters();

  // Order clusters consistent with the original ordering.
  // TODO(rahmanl): Order clusters in decreasing order of their exec density.
  std::sort(clusters.begin(), clusters.end(),
            [](auto &lhs, auto &rhs) { return lhs->id() < rhs->id(); });

  CFGScoreMapTy orig_intra_score_map = ComputeOrigLayoutScores(cfgs_);
  CFGScoreMapTy opt_intra_score_map = ComputeOptLayoutScores(cfgs_, clusters);

  CodeLayoutResult layout_clusters;

  ControlFlowGraph *cfg = nullptr;
  unsigned layout_index = 0;

  // Cold clusters are laid out consistently with how hot clusters appear in the
  // layout. For two functions foo and bar, foo's cold cluster is placed before
  // bar's cold cluster iff (any) hot cluster of foo appears before (all) hot
  // clusters of bar.
  unsigned cold_cluster_layout_index = 0;

  auto cluster_it = layout_clusters.end();

  // Iterate over all CFG nodes in order and add them to the cluster layout
  // information.
  for (auto &cluster : clusters) {
    cluster->VisitEachNodeRef([&](auto &n) {
      if (cfg != n.cfg_ || n.is_entry()) {
        // Switch to the right cluster layout info when the function changes or
        // Or when an entry basic block is reached.
        cfg = n.cfg_;
        uint64_t func_symbol_ordinal = cfg->GetEntryNode()->symbol_ordinal_;
        bool inserted = false;
        std::tie(cluster_it, inserted) = layout_clusters.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(func_symbol_ordinal),
            std::forward_as_tuple(cfg,
                                  orig_intra_score_map.at(cfg),
                                  opt_intra_score_map.at(cfg),
                                  cold_cluster_layout_index));
        if (inserted) ++cold_cluster_layout_index;
        // Start a new cluster and increment the global layout index.
        cluster_it->second.clusters.emplace_back(layout_index++);
      }
      cluster_it->second.clusters.back().bb_indexes.push_back(n.bb_index_);
    });
  }
  return layout_clusters;
}
}  // namespace devtools_crosstool_autofdo
