#include "llvm_propeller_node_chain_builder.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_node_chain.h"
#include "llvm_propeller_node_chain_assembly.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/status/statusor.h"

// This file contains the implementation of the ext-TSP algorithm
// (https://ieeexplore.ieee.org/abstract/document/9050435).
//
// In short, the algorithm iteratively merges chains of basic blocks together to
// form longer chains. At every step the algorithm looks at all ways of merging
// two of the existing chains together and picks and applies the highest-gain
// merge. For merging between two chains, one chain may be split to two chains
// before merging.

namespace devtools_crosstool_autofdo {

using NodeChainAssemblyComparator =
    NodeChainAssembly::NodeChainAssemblyComparator;

void NodeChainBuilder::InitNodeChains() {
  for (ControlFlowGraph *cfg : cfgs_) {
    // TODO(rahmanl) Construct bundles as vector of CFGNodes.
    // For now, make single-node chains for every hot node.
    for (auto &node : cfg->nodes())
      if (node->freq()) {
        auto chain = std::make_unique<NodeChain>(node.get());
        chain->score_ = ComputeScore(*chain);
        uint64_t chain_id = chain->id();
        chains_.try_emplace(chain_id, std::move(chain));
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
  while (!node_chain_assemblies().empty()) {
    MergeChains(GetBestAssembly());
  }
  CoalesceChains();

  std::vector<std::unique_ptr<NodeChain>> chains;
  chains.reserve(chains_.size());
  for (auto &elem : chains_) chains.push_back(std::move(elem.second));
  chains_.clear();
  return chains;
}

// Calculates the total score for a node chain. This function aggregates the
// score of all edges whose src is "this" and sink is "chain".
int64_t NodeChainBuilder::ComputeScore(NodeChain &chain) const {
  int64_t score = 0;
  auto it = chain.out_edges_.find(&chain);
  if (it == chain.out_edges_.end()) return 0;
  for (const CFGEdge *edge : it->second) {
    DCHECK_NE(edge->src()->bundle(), edge->sink()->bundle());
    auto src_offset = GetNodeOffset(edge->src());
    auto sink_offset = GetNodeOffset(edge->sink());
    score += code_layout_scorer_.GetEdgeScore(
        *edge, sink_offset - src_offset - edge->src()->size());
  }
  return score;
}

void NodeChainBuilder::MergeChainEdges(NodeChain &source,
                                       NodeChain &destination) {
  // Add out-edges of the source chain to out-edges of the destination chain.
  for (auto &[chain, edges] : source.out_edges_) {
    NodeChain &c = (chain->id() == source.id()) ? destination : *chain;
    auto result = destination.out_edges_.emplace(&c, edges);
    // If the chain-edge is already present, just add the CFG edges, otherwise
    // add the destination chain to in-edges.
    if (!result.second) {
      result.first->second.insert(result.first->second.end(), edges.begin(),
                                  edges.end());
    } else {
      c.in_edges_.insert(&destination);
    }
    // Remove the source chain from in-edges.
    c.in_edges_.erase(&source);
  }

  // Add in-edges of the source chain to in-edges of destination chain.
  for (auto *c : source.in_edges_) {
    // Self edges were already handled above.
    if (c->id() == source.id()) continue;
    // Move all controlFlowGraph edges from being mapped to the source chain to
    // being mapped to the destination chain.
    auto &source_chain_edges = c->out_edges_[&source];
    auto &destination_chain_edges = c->out_edges_[&destination];

    destination_chain_edges.insert(destination_chain_edges.end(),
                              source_chain_edges.begin(),
                              source_chain_edges.end());
    destination.in_edges_.insert(c);
    // Remove the defunct source chain from out-edges.
    c->out_edges_.erase(&source);
  }
}

void NodeChainBuilder::UpdateNodeChainAssembly(NodeChain &split_chain,
                                               NodeChain &unsplit_chain) {
  absl::StatusOr<NodeChainAssembly> best_assembly =
      NodeChainAssembly::BuildNodeChainAssembly(
          code_layout_scorer_, split_chain, unsplit_chain, MergeOrder::kSU);

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
    if (split_chain.node_bundles_.size() <=
        code_layout_scorer_.code_layout_params().chain_split_threshold()) {
      for (MergeOrder merge_order : {MergeOrder::kS1US2, MergeOrder::kS2S1U,
                                     MergeOrder::kUS2S1, MergeOrder::kS2US1}) {
        for (int slice_pos = 1; slice_pos != split_chain.node_bundles_.size();
             ++slice_pos) {
          // Create the NodeChainAssembly representing this particular assembly.
          compare_and_update_best_assembly(
              NodeChainAssembly::BuildNodeChainAssembly(
                  code_layout_scorer_, split_chain, unsplit_chain, merge_order,
                  slice_pos));
        }
      }
    } else {
      // If split_chain is larger than the threshold, try finding splitting
      // positions based on edges which can be converted to fallthroughs in the
      // new chain.
      auto try_assemblies = [&](int slice_pos,
                                absl::Span<const MergeOrder> merge_orders) {
        if (slice_pos == 0 || slice_pos == split_chain.node_bundles_.size())
          return;
        for (auto merge_order : merge_orders) {
          compare_and_update_best_assembly(
              NodeChainAssembly::BuildNodeChainAssembly(
                  code_layout_scorer_, split_chain, unsplit_chain, merge_order,
                  slice_pos));
        }
      };

      // Find edges from the end of unsplit_chain to the middle of split_chain.
      unsplit_chain.GetLastNode()->ForEachOutEdgeRef([&](const CFGEdge &edge) {
        if (!edge.weight() || edge.IsReturn() || edge.IsCall()) return;
        if (GetNodeChain(edge.sink()).id() != split_chain.id()) return;
        if (edge.sink()->bundle()->nodes_.front() != edge.sink()) return;
        try_assemblies(edge.sink()->bundle()->chain_index_,
                       {MergeOrder::kS1US2, MergeOrder::kUS2S1});
      });

      // Find edges from the middle of split_chain to the beginning of
      // unsplit_chain.
      unsplit_chain.GetFirstNode()->ForEachInEdgeRef([&](const CFGEdge &edge) {
        if (!edge.weight() || edge.IsReturn() || edge.IsCall()) return;
        if (GetNodeChain(edge.src()).id() != split_chain.id()) return;
        if (edge.src()->bundle()->nodes_.back() != edge.src()) return;
        try_assemblies(edge.src()->bundle()->chain_index_ + 1,
                       {MergeOrder::kS1US2, MergeOrder::kS2S1U});
      });
    }
  }

  std::pair<NodeChain *, NodeChain *> chain_pair =
      std::make_pair(&split_chain, &unsplit_chain);
  if (best_assembly.ok()) {
    node_chain_assemblies_.insert_or_assign(chain_pair,
                                            std::move(*best_assembly));
  } else {
    node_chain_assemblies_.erase(chain_pair);
  }
}

// Initializes the chain assemblies (merging candidates) across all the chains.
void NodeChainBuilder::InitChainAssemblies() {
  absl::flat_hash_set<std::pair<NodeChain *, NodeChain *>> visited;
  for (auto &[unused, chain_ptr] : chains_) {
    NodeChain *chain = chain_ptr.get();
    chain->VisitEachCandidateChain(
        [&](NodeChain *other_chain) {
          if (!visited
                   .insert(std::minmax(chain, other_chain,
                                       NodeChain::PtrComparator()))
                   .second)
            return;

          UpdateNodeChainAssembly(*chain, *other_chain);
          UpdateNodeChainAssembly(*other_chain, *chain);
        });
  }
}

void NodeChainBuilder::MergeChains(NodeChain &left_chain,
                                   NodeChain &right_chain) {
  CHECK(&left_chain != &right_chain) << "Cannot merge a chain with itself.";
  CHECK(!right_chain.GetFirstNode()->is_entry())
      << "Should not place entry block in the middle of the chain.";
  // First remove all assemblies associated with right_chain.
  right_chain.VisitEachCandidateChain(
      [this, &right_chain](NodeChain *other_chain) {
        node_chain_assemblies_.erase(std::make_pair(&right_chain, other_chain));
        node_chain_assemblies_.erase(std::make_pair(other_chain, &right_chain));
      });

  // Merge in and out edges of right_chain into those of left_chain.
  MergeChainEdges(right_chain, left_chain);

  // Actually merge the nodes of right_chain into left_chain.
  left_chain.MergeWith(right_chain);

  // Recompute the score for left_chain.
  // TODO(rahmanl): Increment by the inter-chain score.
  left_chain.score_ = ComputeScore(left_chain);
  UpdateAssembliesAfterMerge(left_chain, right_chain);
}

void NodeChainBuilder::MergeChains(NodeChainAssembly assembly) {
  CHECK_GT(assembly.score_gain(), 0);
  NodeChain &split_chain = assembly.split_chain();
  NodeChain &unsplit_chain = assembly.unsplit_chain();
  ++stats_.n_assemblies_by_merge_order[assembly.merge_order()];

  // Merge in and out edges of the unsplit_chain into those of split_chain.
  MergeChainEdges(unsplit_chain, split_chain);

  // Actually merge the nodes of unsplit_chain into split_chain, in the order
  // given by the assembly. The assembly will be dead after the call to
  // `ConsumeEachNodeBundleInAssemblyOrder`.
  std::vector<std::unique_ptr<CFGNodeBundle>> merged_node_bundles;
  int64_t chain_offset = 0;
  int chain_index = 0;
  int64_t assembly_score_gain = assembly.score_gain();
  std::move(assembly).ConsumeEachNodeBundleInAssemblyOrder(
      [&](std::unique_ptr<CFGNodeBundle> node_bundle_ptr) {
        node_bundle_ptr->chain_ = &split_chain;
        node_bundle_ptr->chain_offset_ = chain_offset;
        node_bundle_ptr->chain_index_ = chain_index++;
        chain_offset += node_bundle_ptr->size_;
        merged_node_bundles.push_back(std::move(node_bundle_ptr));
      });

  split_chain.node_bundles_ = std::move(merged_node_bundles);

  // Update the size of the aggregated chain.
  split_chain.size_ += unsplit_chain.size_;
  CHECK_EQ(split_chain.size_, chain_offset);
  // Update the total frequency of the aggregated chain.
  split_chain.freq_ += unsplit_chain.freq_;

  // We have already computed the new score in the assembly record. So we can
  // update the score based on that and the other chain's score.
  split_chain.score_ += unsplit_chain.score_ + assembly_score_gain;
  // In debug mode, check that the new score has been computed correctly.
  DCHECK_EQ(split_chain.score_, ComputeScore(split_chain));

  // Nullify the CFG of the aggregated chain if the other chain comes from a
  // different CFG. This only happens for interprocedural layout and indicates
  // that a chain has nodes from different CFGs.
  if (split_chain.cfg_ && split_chain.cfg_ != unsplit_chain.cfg_)
    split_chain.cfg_ = nullptr;
  UpdateAssembliesAfterMerge(split_chain, unsplit_chain);
}

void NodeChainBuilder::InitChainEdges() {
  // Set up the outgoing edges for every chain
  for (auto &elem : chains_) {
    auto *chain = elem.second.get();
    chain->VisitEachNodeRef([chain](const CFGNode &n) {
      n.ForEachOutEdgeRef([chain](CFGEdge &edge) {
        // Ignore returns and zero-frequency edges.
        // TODO(rahmanl): Remove IsCall() for interp
        if (!edge.weight() || edge.IsReturn() || edge.IsCall()) return;
        // Ignore edges running within the same bundle, as they won't be
        // split.
        if (edge.src()->bundle() == edge.sink()->bundle()) return;
        NodeChain &sink_node_chain = GetNodeChain(edge.sink());
        chain->out_edges_[&sink_node_chain].push_back(&edge);
        sink_node_chain.in_edges_.insert(chain);
      });
    });
  }
}

NodeChainAssembly NodeChainBuilder::GetBestAssembly() const {
  CHECK(!node_chain_assemblies_.empty());
  auto best_assembly = absl::c_max_element(
      node_chain_assemblies_, [](const auto &lhs, const auto &rhs) {
        return NodeChainAssemblyComparator()(lhs.second, rhs.second);
      });
  return best_assembly->second;
}

void NodeChainBuilder::UpdateAssembliesAfterMerge(NodeChain &kept_chain,
                                                  NodeChain &defunct_chain) {
  // Remove all assemblies associated with defunct_chain.
  defunct_chain.VisitEachCandidateChain([&](NodeChain *other_chain) {
    node_chain_assemblies_.erase(std::make_pair(&defunct_chain, other_chain));
    node_chain_assemblies_.erase(std::make_pair(other_chain, &defunct_chain));
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

// Coalesces all hot chains. The chain with the entry block comes first, the
// rest of the chains will be ordered in decreasing order of their execution
// density.
void NodeChainBuilder::CoalesceChains() {
  // Nothing to do when only one chain exists.
  if (chains_.size() <= 1) return;
  std::vector<NodeChain *> chain_order;
  chain_order.reserve(chains_.size());
  for (auto &elem : chains_) chain_order.push_back(elem.second.get());

  std::sort(chain_order.begin(), chain_order.end(),
            [](NodeChain *c1, NodeChain *c2) {
              CHECK_EQ(c1->cfg_, c2->cfg_)
                  << "Attempting to coalesce chains from different CFGs.";
              // Make sure the entry block stays at the front.
              if (c2->GetFirstNode()->is_entry()) return false;
              if (c1->GetFirstNode()->is_entry()) return true;
              // Sort chains in decreasing order of execution density, and
              // break ties according to the initial ordering.
              return std::forward_as_tuple(-c1->exec_density(), c1->id()) <
                     std::forward_as_tuple(-c2->exec_density(), c2->id());
            });

  for (int i = 1; i < chain_order.size(); ++i)
    MergeChains(*chain_order.front(), *chain_order[i]);
}

}  // namespace devtools_crosstool_autofdo
