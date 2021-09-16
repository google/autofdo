#include "llvm_propeller_node_chain_builder.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_node_chain.h"

namespace devtools_crosstool_autofdo {

void NodeChainBuilder::InitNodeChains() {
  for (ControlFlowGraph *cfg : cfgs_) {
    // TODO(rahmanl) Construct bundles as vector of CFGNodes.
    // For now, make single-node chains for every hot node.
    for (auto &node : cfg->nodes())
      if (node->freq()) {
        auto chain = std::make_unique<NodeChain>(node.get());
        chains_.try_emplace(chain->id(), std::move(chain));
      }
  }
}

// This function groups nodes in chains to maximize ExtTSP score and returns the
// constructed chains. After this returns, chains_ becomes empty.
std::vector<std::unique_ptr<NodeChain>>  NodeChainBuilder::BuildChains() {
  InitNodeChains();
  InitChainEdges();
  InitChainAssemblies();
  KeepMergingBestChains();
  CoalesceChains();

  std::vector<std::unique_ptr<NodeChain>> chains;
  for (auto &elem : chains_)
      chains.push_back(std::move(elem.second));
  chains_.clear();
  return chains;
}

// Calculate the total score for a node chain. This function aggregates the
// score of all edges whose src is "this" and sink is "chain".
uint64_t NodeChainBuilder::ComputeScore(NodeChain *chain) const {
  uint64_t score = 0;

  chain->VisitEachOutEdgeToChain(chain, [&](const CFGEdge &edge) {
    DCHECK_NE(edge.src()->bundle(), edge.sink()->bundle());
    auto src_offset = GetNodeOffset(edge.src());
    auto sink_offset = GetNodeOffset(edge.sink());
    score += code_layout_scorer_.GetEdgeScore(edge,
                          sink_offset - src_offset - edge.src()->size());
  });

  return score;
}

// This function merges the in-and-out chain-edges of one chain (source)
// into those of another (destination).
void NodeChainBuilder::MergeChainEdges(NodeChain *source,
                                       NodeChain *destination) {
  // Add out-edges of the source chain to out-edges of the destination chain.
  for (auto &elem : source->out_edges_) {
    NodeChain *c = (elem.first == source) ? destination : elem.first;
    auto res = destination->out_edges_.emplace(c, elem.second);
    // If the chain-edge is already present, just add the CFG edges, otherwise
    // add the destination chain to in-edges.
    if (!res.second)
      res.first->second.insert(res.first->second.end(), elem.second.begin(),
                               elem.second.end());
    else
      c->in_edges_.insert(destination);

    // Remove the source chain from in-edges.
    c->in_edges_.erase(source);
  }

  // Add in-edges of the source chain to in-edges of destination chain.
  for (auto *c : source->in_edges_) {
    // Self edges were already handled above.
    if (c == source) continue;
    // Move all controlFlowGraph edges from being mapped to the source chain to
    // being mapped to the destination chain.
    auto &source_chain_edges = c->out_edges_[source];
    auto &destination_chain_edges = c->out_edges_[destination];

    destination_chain_edges.insert(destination_chain_edges.end(),
                              source_chain_edges.begin(),
                              source_chain_edges.end());
    destination->in_edges_.insert(c);
    // Remove the defunct source chain from out-edges.
    c->out_edges_.erase(source);
  }
}

void NodeChainBuilder::UpdateNodeChainAssembly(NodeChain *left_chain,
                                               NodeChain *right_chain) {
  // Discard the chain assembly if it puts the entry node in the middle.
  if (right_chain->GetFirstNode()->is_entry())
    return;

  uint64_t score = 0;

  auto addEdgeScore = [&](const CFGEdge &edge) {
    auto *src_chain = GetNodeChain(edge.src());
    auto src_offset = GetNodeOffset(edge.src());
    auto sink_offset = GetNodeOffset(edge.sink());
    if (src_chain == left_chain)
      sink_offset += left_chain->size_;
    else
      src_offset += left_chain->size_;
    int64_t src_sink_offset = sink_offset - src_offset - edge.src()->size();
    score += code_layout_scorer_.GetEdgeScore(edge, src_sink_offset);
  };

  // Add up the score contribution from edges between left_chain and
  // right_chain.
  left_chain->VisitEachOutEdgeToChain(right_chain, addEdgeScore);
  right_chain->VisitEachOutEdgeToChain(left_chain, addEdgeScore);

  // If score is non-zero, update the assembly map entry, otherwise make sure we
  // remove the associated entry.
  if (score != 0)
    node_chain_assemblies_[std::make_pair(left_chain, right_chain)] = score;
  else
    node_chain_assemblies_.erase(std::make_pair(left_chain, right_chain));
}

// Initializes the chain assemblies across all the chains.
void NodeChainBuilder::InitChainAssemblies() {
  std::set<std::pair<NodeChain *, NodeChain *>> visited;

  for (auto &elem : chains_) {
    NodeChain *chain = elem.second.get();
    chain->VisitEachCandidateChain(
        [this, &visited, chain](NodeChain *other_chain) {
          if (!visited
                   .insert(std::minmax(chain, other_chain,
                                       NodeChain::PtrComparator()))
                   .second)
            return;

          UpdateNodeChainAssembly(chain, other_chain);
          UpdateNodeChainAssembly(other_chain, chain);
        });
  }
}

void NodeChainBuilder::MergeChains(NodeChain *left_chain,
                                   NodeChain *right_chain) {
  CHECK(left_chain != right_chain) << "Cannot merge a chain with itself.";
  CHECK(!right_chain->GetFirstNode()->is_entry())
      << "Should not place entry block in the middle of the chain.";
  // First remove all assemblies associated with right_chain.
  right_chain->VisitEachCandidateChain(
      [this, right_chain](NodeChain *other_chain) {
        node_chain_assemblies_.erase(std::make_pair(right_chain, other_chain));
        node_chain_assemblies_.erase(std::make_pair(other_chain, right_chain));
      });

  // Merge in and out edges of right_chain into those of left_chain.
  MergeChainEdges(right_chain, left_chain);

  // Actually merge the nodes of right_chain into left_chain.
  left_chain->MergeWith(right_chain);

  // right_chain is now defunct and should be removed from chains.
  chains_.erase(right_chain->id());

  // Update assemblies associated with left_chain as their score may have been
  // changed by the merging.
  left_chain->VisitEachCandidateChain(
      [this, left_chain](NodeChain *other_chain) {
        UpdateNodeChainAssembly(left_chain, other_chain);
        UpdateNodeChainAssembly(other_chain, left_chain);
      });
}

void NodeChainBuilder::InitChainEdges() {
  // Set up the outgoing edges for every chain
  for (auto &elem : chains_) {
    auto *chain = elem.second.get();
    chain->VisitEachNodeRef([chain](auto &n) {
      n.ForEachOutEdgeRef([chain](CFGEdge &edge) {
        // Ignore returns and zero-frequency edges.
        // TODO(rahmanl): Remove IsCall() for interp
        if (!edge.weight() || edge.IsReturn() || edge.IsCall()) return;
        // Ignore edges running within the same bundle, as they won't be split.
        if (edge.src()->bundle() == edge.sink()->bundle()) return;
        auto *sink_node_chain = GetNodeChain(edge.sink());
        chain->out_edges_[sink_node_chain].push_back(&edge);
        sink_node_chain->in_edges_.insert(chain);
      });
    });
  }
}

// This method iteratively merges chains together by taking the highest-score
// chain assembly at every iteration and merging the involved chains.
void NodeChainBuilder::KeepMergingBestChains() {
  // Keep merging chains together until no more score gain can be achieved.
  while (!node_chain_assemblies_.empty()) {
    // Find the assembly which brings about the maximum gain in score.
    auto best_assembly = std::max_element(
        node_chain_assemblies_.begin(), node_chain_assemblies_.end(),
        [](auto &lhs, auto &rhs) {
          if (lhs.second != rhs.second)
            return lhs.second < rhs.second;
          // Consistently break ties when the score is the same.
          if (lhs.first.first->id() != rhs.first.first->id())
            return lhs.first.first->id() < rhs.first.first->id();
          return lhs.first.second->id() < rhs.first.second->id();
        });
    if (best_assembly != node_chain_assemblies_.end()) {
      MergeChains(best_assembly->first.first, best_assembly->first.second);
    }
  }
}

// This function sorts bb chains in decreasing order of their execution density.
// NodeChainBuilder calls this function at the end to ensure that hot bb chains
// are placed at the beginning of the function.
void NodeChainBuilder::CoalesceChains() {
  // Nothing to do when only one chain exists.
  if (chains_.size() <= 1)
    return;
  std::vector<NodeChain *> chain_order;
  for (auto &elem : chains_)
    chain_order.push_back(elem.second.get());

  std::sort(chain_order.begin(), chain_order.end(),
            [](NodeChain *c1, NodeChain *c2) {
              CHECK(c1->cfg_ == c2->cfg_)
                  << "Attempting to coalesce chains from different CFGs.";
              // Make sure the entry block stays at the front.
              if (c1->GetFirstNode()->is_entry())
                return true;
              if (c2->GetFirstNode()->is_entry())
                return false;
              // Sort chains in decreasing order of execution density, and break
              // ties according to the initial ordering.
              if (c1->exec_density() == c2->exec_density())
                return c1->id() < c2->id();
              return c1->exec_density() > c2->exec_density();
            });

  for (int i = 1; i < chain_order.size(); ++i)
    MergeChains(chain_order.front(), chain_order[i]);
}

}  // namespace devtools_crosstool_autofdo
