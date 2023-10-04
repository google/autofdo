#include "llvm_propeller_node_chain_builder.h"

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_merge_order.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_node_chain.h"
#include "llvm_propeller_node_chain_assembly.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/btree_set.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/log/log.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/types/span.h"
#include "math_util.h"

// This file contains the implementation of the ext-TSP algorithm
// (https://ieeexplore.ieee.org/abstract/document/9050435).
//
// In short, the algorithm iteratively merges chains of basic blocks together to
// form longer chains. At every step the algorithm looks at all ways of merging
// two of the existing chains together and picks and applies the highest-gain
// merge. For merging between two chains, one chain may be split to two chains
// before merging.

namespace devtools_crosstool_autofdo {
namespace {

// Returns the initial chains for the given `cfgs`. More specifically, returns
// the subset of elements in `initial_chains` whose key is equal to
// `function_index` of some CFG in `cfgs`.
absl::flat_hash_map<int, std::vector<std::vector<CFGNode::IntraCfgId>>>
GetInitialChainsForCfgs(
    const std::vector<const ControlFlowGraph *> &cfgs,
    const absl::flat_hash_map<
        int, std::vector<std::vector<CFGNode::IntraCfgId>>> &initial_chains) {
  absl::flat_hash_map<int, std::vector<std::vector<CFGNode::IntraCfgId>>>
      result;
  for (const ControlFlowGraph *cfg : cfgs) {
    auto it = initial_chains.find(cfg->function_index());
    if (it == initial_chains.end()) continue;
    result.emplace(cfg->function_index(), it->second);
  }
  return result;
}
}  // namespace

template <class AssemblyQueueImpl>
NodeChainBuilder NodeChainBuilder::CreateNodeChainBuilder(
    const PropellerCodeLayoutScorer &scorer,
    const std::vector<const ControlFlowGraph *> &cfgs,
    const absl::flat_hash_map<
        int, std::vector<std::vector<CFGNode::IntraCfgId>>> &initial_chains,
    PropellerStats::CodeLayoutStats &stats) {
  return NodeChainBuilder(scorer, cfgs,
                          GetInitialChainsForCfgs(cfgs, initial_chains), stats,
                          std::make_unique<AssemblyQueueImpl>());
}

// Explicit instantiation of CreateNodeChainBuilder for both AssemblyQueueImpl
// types.
template NodeChainBuilder
NodeChainBuilder::CreateNodeChainBuilder<NodeChainAssemblyIterativeQueue>(
    const PropellerCodeLayoutScorer &scorer,
    const std::vector<const ControlFlowGraph *> &cfgs,
    const absl::flat_hash_map<
        int, std::vector<std::vector<CFGNode::IntraCfgId>>> &initial_chains,
    PropellerStats::CodeLayoutStats &stats);

template NodeChainBuilder
NodeChainBuilder::CreateNodeChainBuilder<NodeChainAssemblyBalancedTreeQueue>(
    const PropellerCodeLayoutScorer &scorer,
    const std::vector<const ControlFlowGraph *> &cfgs,
    const absl::flat_hash_map<
        int, std::vector<std::vector<CFGNode::IntraCfgId>>> &initial_chains,
    PropellerStats::CodeLayoutStats &stats);

using NodeChainAssemblyComparator =
    NodeChainAssembly::NodeChainAssemblyComparator;

absl::btree_map<const CFGNode *, const CFGNode *, CFGNodePtrComparator>
GetForcedEdges(const ControlFlowGraph &cfg) {
  // Each node can participate in a forced edge at most one time as the source
  // at most one time as the sink.
  // So we compute these edges as a map from source to sink nodes below.
  // As we iterate over CFG edges, we insert their source and sinks in this map.
  // When we visit the second edge for a source node, we nullify the mapped sink
  // node to indicate that the source has multiple outgoing edges and will not
  // have a forced edge. We also keep track of the in-degree of each node.
  // After visiting all edges, we remove all source-sink pairs where
  // sink == nullptr or if sink has an in-degree larger than one.
  absl::btree_map<const CFGNode *, const CFGNode *, CFGNodePtrComparator>
      forced_edges;
  // This stores the number of hot (non-zero weight) incoming edges to every
  // node (hot in-degree).
  absl::flat_hash_map<const CFGNode *, int> hot_in_degree;
  for (const std::unique_ptr<CFGEdge> &edge : cfg.intra_edges()) {
    if (edge->weight() == 0 || edge->IsCall() || edge->IsReturn()) continue;
    auto [it, inserted] = forced_edges.emplace(edge->src(), edge->sink());
    // Nullify the edge for the source node if an out-edge has already been
    // found, indicating that this node does not have a forced edge.
    if (!inserted) it->second = nullptr;
    ++hot_in_degree[edge->sink()];
  }

  // Remove from `forced_edges` all edges which sink to nullptr, besides
  // those which sink to a node having more than one incoming (hot) edge.
  for (auto it = forced_edges.begin(); it != forced_edges.end();) {
    if (!it->second || hot_in_degree[it->second] > 1)
      it = forced_edges.erase(it);
    else
      ++it;
  }
  return forced_edges;
}

void BreakCycles(absl::btree_map<const CFGNode *, const CFGNode *,
                                 CFGNodePtrComparator> &path_next) {
  // This maps each node to the path (id) they belong to. Path ids are 1-based.
  absl::flat_hash_map<const CFGNode *, int> node_to_path_id;
  // These are the source nodes for all edges that must be cut to break cycles.
  std::vector<const CFGNode *> cycle_cut_nodes;
  int current_path_id = 0;
  for (auto it = path_next.begin(); it != path_next.end(); ++it) {
    // Continue if the node and its path/cycle have already been visited.
    if (node_to_path_id.contains(it->first)) continue;
    const CFGNode *victim_node = nullptr;
    // Start traversing the path from this node to find the victim node.
    auto path_visit_it = it;
    ++current_path_id;
    while (path_visit_it != path_next.end()) {
      auto [node_to_path_id_it, inserted] =
          node_to_path_id.emplace(path_visit_it->first, current_path_id);
      if (!inserted) {
        // If this is already mapped to a path, either it is the same number --
        // in which case we have found a cycle, or it is a different number --
        // which means we have found a path to a previously visited path
        // (non-cycle).
        if (node_to_path_id_it->second == current_path_id) {
          // We have found a cycle: add the victim edge
          cycle_cut_nodes.push_back(victim_node);
        }
        break;
      }
      if (!victim_node || (path_visit_it->second->bb_index() <
                           path_next[victim_node]->bb_index())) {
        victim_node = path_visit_it->first;
      }
      // Proceed to the next node in the path.
      path_visit_it = path_next.find(path_visit_it->second);
    }
  }
  // Remove the victim nodes from the path_next map to break cycles.
  for (const CFGNode *node : cycle_cut_nodes) path_next.erase(node);
}

std::vector<std::vector<const CFGNode *>> GetForcedPaths(
    const ControlFlowGraph &cfg) {
  // First find all forced fallthrough edges. Each of these edges are the only
  // edge to their sink and the only edge from their source.
  absl::btree_map<const CFGNode *, const CFGNode *, CFGNodePtrComparator>
      path_next = GetForcedEdges(cfg);

  // Construct paths from `path_next` after breaking its cycles.
  BreakCycles(path_next);

  // Find the beginning nodes of paths.
  absl::btree_set<const CFGNode *, CFGNodePtrComparator> path_begin_nodes;
  for (const auto [source, unused] : path_next) path_begin_nodes.insert(source);
  for (const auto [unused, sink] : path_next) path_begin_nodes.erase(sink);

  std::vector<std::vector<const CFGNode *>> paths;
  for (const CFGNode *begin_node : path_begin_nodes) {
    // Follow the path specified by bundle_next from this node.
    std::vector<const CFGNode *> path = {begin_node};
    for (auto it = path_next.find(begin_node); it != path_next.end();
         it = path_next.find(it->second))
      path.push_back(it->second);
    CHECK_GT(path.size(), 1) << "Paths should have more than one node.";
    paths.push_back(std::move(path));
  }
  return paths;
}

void NodeChainBuilder::InitNodeChains() {
  auto AddNewChain = [&](std::vector<const CFGNode *> nodes,
                         NodeChain::BundleMode bundle_mode) {
    if (nodes.empty()) return;
    if (nodes.size() == 1) {
      ++stats_.n_single_node_chains;
    } else {
      ++stats_.n_multi_node_chains;
    }
    auto chain = std::make_unique<NodeChain>(std::move(nodes), bundle_mode);
    for (std::unique_ptr<CFGNodeBundle> &bundle :
         chain->mutable_node_bundles()) {
      int bundle_offset = 0;
      for (const CFGNode *node : bundle->nodes()) {
        CHECK_EQ(node_to_bundle_mapper_->GetBundleMappingEntry(node).bundle,
                 nullptr)
            << "Node " << node->inter_cfg_id() << " is already in a bundle";
        node_to_bundle_mapper_->SetBundleMappingEntry(
            node, {.bundle = bundle.get(), .bundle_offset = bundle_offset});
        bundle_offset += node->size();
      }
    }
    CFGNode::InterCfgId chain_id = chain->id();
    CHECK(chains_.emplace(chain_id, std::move(chain)).second)
        << "Duplicate chain id: " << chain_id;
  };

  for (const ControlFlowGraph *cfg : cfgs_) {
    if (!cfg->is_hot()) {
      LOG(INFO) << "Function is not hot: \"" << cfg->GetPrimaryName().str()
              << "\".";
      continue;
    }
    auto it = initial_chains_.find(cfg->function_index());
    const std::vector<std::vector<CFGNode::IntraCfgId>> &cfg_initial_chains =
        it != initial_chains_.end()
            ? it->second
            : std::vector<std::vector<CFGNode::IntraCfgId>>{};
    bool has_hot_landing_pads = cfg->has_hot_landing_pads();
    // In `inter_function_reordering` mode, check if we must coalesce the
    // landing pads for `cfg` which happens when `cfg` has more than one landing
    // pads and they are not all cold. Note: All landing pads of each function
    // must be placed in the same section to comply with C++ exception handling
    // API requirement. In `inter_function_reordering` mode, if all landing pads
    // of a function are cold or if it has a single landing pad, then we are
    // fine. Otherwise, if the computed layout places them in different
    // `NodeChain`s, they may potentially be spread across different sections.
    // Even though in such cases, LLVM will fix the ordering by placing all
    // landing pads in the ".text.eh.<func_name>" section, this might be
    // suboptimal. So here, we fall back to intra-function ordering by calling
    // `CreateNodeChinaBuilder` on this single CFG which will coalesce all
    // chains together. Then we make a `CFGNodeBundle` from the coalesced
    // `NodeChain` to make sure it won't be split by inter-procedural layout.
    // TODO(b/159842094): Only merge chains with landing pads.
    if (code_layout_scorer_.code_layout_params().inter_function_reordering() &&
        cfgs_.size() > 1 &&
        // `cfg` has more than one landing pads and they are not all cold.
        cfg->n_landing_pads() > 1 && has_hot_landing_pads) {
      auto chains = CreateNodeChainBuilder(
                        code_layout_scorer_, {cfg},
                        {{cfg->function_index(), cfg_initial_chains}}, stats_)
                        .BuildChains();
      for (const std::unique_ptr<NodeChain> &chain : chains) {
        std::vector<const CFGNode *> chain_nodes;
        for (std::unique_ptr<CFGNodeBundle> &bundle :
             chain->mutable_node_bundles()) {
          for (const CFGNode *node : bundle->nodes())
            chain_nodes.push_back(node);
        }
        AddNewChain(std::move(chain_nodes),
                    NodeChain::BundleMode::kBundleAllNodesTogether);
      }
      continue;
    }
    std::vector<const CFGNode *> hot_nodes_in_order;
    std::vector<const CFGNode *> cold_nodes_in_order;
    for (const std::unique_ptr<CFGNode> &node : cfg->nodes()) {
      if (node->CalculateFrequency() != 0 ||
          // Assume the entry block hot in non-inter-procedural mode.
          (node->is_entry() && !code_layout_scorer_.code_layout_params()
                                    .inter_function_reordering()) ||
          // Assume cold landing pad blocks hot if `cfg` has at least one hot
          // landing pad. We need this to make sure `CoalesceChains` merges
          // all landing pads into a single chain.
          (node->is_landing_pad() && has_hot_landing_pads)) {
        hot_nodes_in_order.push_back(node.get());
      } else {
        cold_nodes_in_order.push_back(node.get());
      }
    }

    // When `split_functions=false`, build a chain for all the cold nodes so it
    // can be merged with the other nodes to build a single chain without
    // splitting the cold part.
    if (!code_layout_scorer_.code_layout_params().split_functions())
      AddNewChain(std::move(cold_nodes_in_order),
                  NodeChain::BundleMode::kBundleAllNodesTogether);

    // When `reorder_blocks=false`, build a single chain for all hot nodes to
    // keep their relative order. In this case no other chains need to be
    // built for this CFG.
    if (!code_layout_scorer_.code_layout_params().reorder_hot_blocks()) {
      CHECK(!hot_nodes_in_order.empty())
          << "Function \"" << cfg->GetPrimaryName().str()
          << "\" has no hot blocks.";
      AddNewChain(std::move(hot_nodes_in_order),
                  NodeChain::BundleMode::kBundleAllNodesTogether);
      continue;
    }
    // Construct the initial chains if requested.
    if (!cfg_initial_chains.empty()) {
      for (const auto &chain : cfg_initial_chains) {
        std::vector<const CFGNode *> node_chain;
        node_chain.reserve(chain.size());
        for (const CFGNode::IntraCfgId &intra_cfg_id : chain)
          node_chain.push_back(&cfg->GetNodeById(intra_cfg_id));
        // These chains won't be bundled (They can be split later).
        AddNewChain(std::move(node_chain),
                    NodeChain::BundleMode::kBundleEachSingleNode);
      }
    } else {
      // Construct bundled node chains for the paths.
      for (auto &path : GetForcedPaths(*cfg))
        AddNewChain(std::move(path),
                    NodeChain::BundleMode::kBundleAllNodesTogether);
    }

    // Make single-node chains for the remaining hot nodes.
    for (const CFGNode *node : hot_nodes_in_order) {
      if (node_to_bundle_mapper_->GetBundleMappingEntry(node).bundle != nullptr)
        continue;
      AddNewChain({node}, NodeChain::BundleMode::kBundleEachSingleNode);
    }
  }
}

// This function groups nodes in chains to maximize ExtTSP score and returns the
// constructed chains. After this returns, chains_ becomes empty.
std::vector<std::unique_ptr<NodeChain>> NodeChainBuilder::BuildChains() {
  InitNodeChains();
  InitChainEdges();
  InitChainAssemblies();
  // Keep merging chains together until no more score gain can be achieved.
  while (!node_chain_assemblies_->empty()) {
    MergeChains(node_chain_assemblies_->GetBestAssembly());
  }

  // Merge all chains into a if we only have a single cfg.
  if (cfgs_.size() == 1) CoalesceChains();

  std::vector<std::unique_ptr<NodeChain>> chains;
  chains.reserve(chains_.size());
  for (auto &[unused, chain] : chains_) chains.push_back(std::move(chain));
  chains_.clear();
  return chains;
}

void NodeChainBuilder::UpdateNodeChainAssembly(NodeChain &split_chain,
                                               NodeChain &unsplit_chain) {
  absl::StatusOr<NodeChainAssembly> best_assembly =
      NodeChainAssembly::BuildNodeChainAssembly(
          *node_to_bundle_mapper_, code_layout_scorer_, split_chain,
          unsplit_chain, {.merge_order = ChainMergeOrder::kSU});

  if (code_layout_scorer_.code_layout_params().chain_split()) {
    auto compare_and_update_best_assembly =
        [&](absl::StatusOr<NodeChainAssembly> assembly) {
          if (!assembly.ok()) return;
          if (!best_assembly.ok() ||
              NodeChainAssemblyComparator()(*best_assembly, *assembly)) {
            best_assembly = std::move(assembly);
          }
        };

    // Consider splitting split_chain at every position if the number of bundles
    // does not exceed the splitting threshold.
    if (split_chain.node_bundles().size() <=
        code_layout_scorer_.code_layout_params().chain_split_threshold()) {
      for (ChainMergeOrder merge_order :
           {ChainMergeOrder::kS1US2, ChainMergeOrder::kS2S1U,
            ChainMergeOrder::kUS2S1, ChainMergeOrder::kS2US1}) {
        for (int slice_pos = 1; slice_pos != split_chain.node_bundles().size();
             ++slice_pos) {
          // Create the NodeChainAssembly representing this particular assembly.
          compare_and_update_best_assembly(
              NodeChainAssembly::BuildNodeChainAssembly(
                  *node_to_bundle_mapper_, code_layout_scorer_, split_chain,
                  unsplit_chain,
                  {.merge_order = merge_order, .slice_pos = slice_pos}));
        }
      }
    } else {
      // If split_chain is larger than the threshold, try finding splitting
      // positions based on edges which can be converted to fallthroughs in the
      // new chain.
      auto try_assemblies =
          [&](int slice_pos, absl::Span<const ChainMergeOrder> merge_orders) {
            if (slice_pos == 0 ||
                slice_pos == split_chain.node_bundles().size())
              return;
            for (auto merge_order : merge_orders) {
              compare_and_update_best_assembly(
                  NodeChainAssembly::BuildNodeChainAssembly(
                      *node_to_bundle_mapper_, code_layout_scorer_, split_chain,
                      unsplit_chain,
                      {.merge_order = merge_order, .slice_pos = slice_pos}));
            }
          };

      // Find edges from the end of unsplit_chain to the middle of split_chain.
      unsplit_chain.GetLastNode()->ForEachOutEdgeRef([&](const CFGEdge &edge) {
        if (!ShouldVisitEdge(edge)) return;
        const CFGNodeBundle *sink_node_bundle =
            node_to_bundle_mapper_->GetBundleMappingEntry(edge.sink()).bundle;
        if (sink_node_bundle->chain_mapping().chain->id() != split_chain.id())
          return;
        if (sink_node_bundle->nodes().front() != edge.sink()) return;
        try_assemblies(sink_node_bundle->chain_mapping().chain_index,
                       {ChainMergeOrder::kS1US2, ChainMergeOrder::kUS2S1});
      });

      // Find edges from the middle of split_chain to the beginning of
      // unsplit_chain.
      unsplit_chain.GetFirstNode()->ForEachInEdgeRef([&](const CFGEdge &edge) {
        if (!ShouldVisitEdge(edge)) return;
        const CFGNodeBundle *src_node_bundle =
            node_to_bundle_mapper_->GetBundleMappingEntry(edge.src()).bundle;
        if (src_node_bundle->chain_mapping().chain->id() != split_chain.id())
          return;
        if (src_node_bundle->nodes().back() != edge.src()) return;
        try_assemblies(src_node_bundle->chain_mapping().chain_index + 1,
                       {ChainMergeOrder::kS1US2, ChainMergeOrder::kS2S1U});
      });
    }
  }
  if (best_assembly.ok()) {
    node_chain_assemblies_->InsertAssembly(std::move(*best_assembly));
  } else {
    node_chain_assemblies_->RemoveAssembly(
        {.split_chain = &split_chain, .unsplit_chain = &unsplit_chain});
  }
}

// Initializes the chain assemblies (merging candidates) across all the chains.
void NodeChainBuilder::InitChainAssemblies() {
  for (auto &[unused, chain_ptr] : chains_) {
    NodeChain *chain = chain_ptr.get();
    chain->VisitEachCandidateChain([&](NodeChain *other_chain) {
      // `UpdateNodeChainAssembly(*other_chain, *chain)` is invoked when
      // visiting candidate chains for `other_chain`.
      UpdateNodeChainAssembly(*chain, *other_chain);
    });
  }
}

void NodeChainBuilder::MergeChains(NodeChain &left_chain,
                                   NodeChain &right_chain) {
  absl::StatusOr<NodeChainAssembly> assembly =
      NodeChainAssembly::BuildNodeChainAssembly(
          *node_to_bundle_mapper_, code_layout_scorer_, left_chain, right_chain,
          {.merge_order = ChainMergeOrder::kSU,
           .error_on_zero_score_gain = false});
  CHECK_OK(assembly);
  MergeChains(*std::move(assembly));
}

void NodeChainBuilder::MergeChains(NodeChainAssembly assembly) {
  CHECK_GE(assembly.score_gain(), 0);
  NodeChain &split_chain = assembly.split_chain();
  NodeChain &unsplit_chain = assembly.unsplit_chain();
  ++stats_.n_assemblies_by_merge_order[assembly.merge_order()];

  split_chain.MergeWith(std::move(assembly), *node_to_bundle_mapper_,
                        code_layout_scorer_);

  // In debug mode, check that the new score has been computed correctly.
  DCHECK(MathUtil::AlmostEquals(
      split_chain.score(),
      split_chain.ComputeScore(*node_to_bundle_mapper_, code_layout_scorer_)));

  UpdateAssembliesAfterMerge(split_chain, unsplit_chain);
}

void NodeChainBuilder::InitChainEdges() {
  // Set up the outgoing edges for every chain
  for (auto &[unused, chain_ptr] : chains_) {
    NodeChain *chain = chain_ptr.get();
    chain->VisitEachNodeRef([&](const CFGNode &n) {
      n.ForEachOutEdgeRef([&](const CFGEdge &edge) {
        if (!ShouldVisitEdge(edge)) return;
        CFGNodeBundle *src_node_bundle =
            node_to_bundle_mapper_->GetBundleMappingEntry(edge.src()).bundle;
        const CFGNodeBundle *sink_node_bundle =
            node_to_bundle_mapper_->GetBundleMappingEntry(edge.sink()).bundle;
        // Ignore edges running within the same bundle, as they won't be split.
        if (src_node_bundle == sink_node_bundle) return;
        NodeChain *sink_node_chain = sink_node_bundle->chain_mapping().chain;
        if (sink_node_chain == chain) {
          src_node_bundle->mutable_intra_chain_out_edges().push_back(&edge);
          return;
        }
        auto [it, inserted] = chain->mutable_inter_chain_out_edges().emplace(
            sink_node_chain, std::vector<const CFGEdge *>(1, &edge));
        if (inserted)
          sink_node_chain->mutable_inter_chain_in_edges().insert(chain);
        else
          it->second.push_back(&edge);
      });
    });
  }
  for (auto &[chain_id, chain] : chains_) {
    chain->SetScore(
        chain->ComputeScore(*node_to_bundle_mapper_, code_layout_scorer_));
  }
}

void NodeChainBuilder::UpdateAssembliesAfterMerge(NodeChain &kept_chain,
                                                  NodeChain &defunct_chain) {
  // Remove all assemblies associated with defunct_chain.
  defunct_chain.VisitEachCandidateChain([&](NodeChain *other_chain) {
    node_chain_assemblies_->RemoveAssembly(
        {.split_chain = &defunct_chain, .unsplit_chain = other_chain});
    node_chain_assemblies_->RemoveAssembly(
        {.split_chain = other_chain, .unsplit_chain = &defunct_chain});
  });

  // Remove the defunct chain.
  chains_.erase(defunct_chain.id());

  // Update assemblies associated with split_chain as their score may have
  // changed by the merge.
  kept_chain.VisitEachCandidateChain([&](NodeChain *other_chain) {
    UpdateNodeChainAssembly(kept_chain, *other_chain);
    UpdateNodeChainAssembly(*other_chain, kept_chain);
  });
}

// Coalesces the chains to a single chain. The chain with the entry block comes
// first, the rest of the chains will be ordered in decreasing order
// of their execution density. All chains in `chains_` must be from the same
// function.
void NodeChainBuilder::CoalesceChains() {
  CHECK_EQ(cfgs_.size(), 1);
  if (chains_.empty()) {
    LOG(WARNING) << "Coalescing for function: " << cfgs_[0]->function_index()
                 << " no chains.";
    return;
  }
  std::vector<NodeChain *> chain_order;
  chain_order.reserve(chains_.size());
  for (auto &[chain_id, chain] : chains_) chain_order.push_back(chain.get());

  // Sort chains in the order they should be merged together. Chains are ordered
  // in decreasing order of their execution density with a special case for the
  // function entry chain which must be placed at the front.
  absl::c_sort(chain_order, [](const NodeChain *c1, const NodeChain *c2) {
    // Make sure the entry block is placed at the front.
    if (c2->GetFirstNode()->is_entry()) return false;
    if (c1->GetFirstNode()->is_entry()) return true;
    // Sort chains in decreasing order of execution density, and break ties
    // according to the initial ordering.
    return std::make_tuple(-c1->exec_density(), c1->id()) <
           std::make_tuple(-c2->exec_density(), c2->id());
  });

  NodeChain *coalesced_chain = chain_order.front();
  // Merge runs of consecutive chains from the same CFGs.
  for (auto *chain : chain_order) {
    if (chain == coalesced_chain) continue;
    MergeChains(*coalesced_chain, *chain);
  }
}

}  // namespace devtools_crosstool_autofdo
