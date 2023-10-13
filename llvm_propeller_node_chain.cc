#include "llvm_propeller_node_chain.h"

#include <iterator>
#include <tuple>
#include <utility>

#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain_assembly.h"
#include "llvm_propeller_options.pb.h"
#include "third_party/abseil/absl/algorithm/container.h"

namespace devtools_crosstool_autofdo {

namespace {
// Returns whether merging chains in `assembly` requires rebundling their
// `node_bundles_`.
bool ShouldRebundle(const NodeChainAssembly &assembly,
                    const PropellerCodeLayoutParameters &code_layout_params) {
  return (assembly.split_chain().cfg_ == nullptr ||
          (assembly.split_chain().cfg_ != assembly.unsplit_chain().cfg_)) &&
         code_layout_params.chain_split() &&
         assembly.split_chain().node_bundles_.size() +
                 assembly.unsplit_chain().node_bundles_.size() >
             code_layout_params.chain_split_threshold();
}
}  // namespace

NodeChain &GetNodeChain(const CFGNode *n) {
  CHECK_NE(n->bundle(), nullptr);
  CHECK_NE(n->bundle()->chain_, nullptr);
  return *n->bundle()->chain_;
}

int64_t GetNodeOffset(const CFGNode *n) {
  CHECK(n->bundle()) << "Chain not initialized for node.";
  return n->bundle()->chain_offset_ + n->bundle_offset();
}

void NodeChain::MergeWith(NodeChainAssembly assembly,
                          const PropellerCodeLayoutScorer &code_layout_scorer) {
  CHECK(this == &assembly.split_chain());
  NodeChain &unsplit_chain = assembly.unsplit_chain();

  // Merge in and out edges of the `unsplit_chain` into those of `*this`.
  MergeChainEdges(unsplit_chain);

  // Update the size of the aggregated chain.
  size_ += unsplit_chain.size_;
  // Update the total frequency of the aggregated chain.
  freq_ += unsplit_chain.freq_;

  // We have already computed the new score in the assembly record. So we can
  // update the score based on that and the other chain's score.
  score_ += unsplit_chain.score_ + assembly.score_gain();

  // Actually merge the nodes of `unsplit_chain` into `*this`, in the order
  // given by `assembly`. `assembly` will be dead after the call to
  // `ConsumeEachNodeBundleInAssemblyOrder`.
  std::vector<std::unique_ptr<CFGNodeBundle>> merged_node_bundles;
  int64_t chain_offset = 0;
  int chain_index = 0;
  CFGNodeBundle *prev_bundle = nullptr;
  bool should_rebundle =
      ShouldRebundle(assembly, code_layout_scorer.code_layout_params());
  int n_bundles_pre_merge = assembly.split_chain().node_bundles_.size() +
                            assembly.unsplit_chain().node_bundles_.size();
  std::move(assembly).ConsumeEachNodeBundleInAssemblyOrder(
      [&](std::unique_ptr<CFGNodeBundle> node_bundle) {
        // Try rebundling if this bundle comes from the same CFG as the previous
        // one.
        if (should_rebundle && prev_bundle != nullptr &&
            prev_bundle->nodes_.front()->cfg() ==
             node_bundle->nodes_.front()->cfg()) {
          chain_offset += node_bundle->size_;
          prev_bundle->MergeWith(std::move(*node_bundle));
        } else {
          node_bundle->chain_ = this;
          node_bundle->chain_offset_ = chain_offset;
          node_bundle->chain_index_ = chain_index++;
          chain_offset += node_bundle->size_;
          prev_bundle = node_bundle.get();
          merged_node_bundles.push_back(std::move(node_bundle));
        }
      });

  node_bundles_ = std::move(merged_node_bundles);

  // Remove intra-bundle edges if we have actually rebundled any chain, i.e.,
  // if the `node_bundles_.size()` has changed since before the merge.
  if (n_bundles_pre_merge != node_bundles_.size())
    RemoveIntraBundleEdges(code_layout_scorer);
  SortIntraChainEdges();

  // Nullify the CFG of the aggregated chain if the other chain comes from a
  // different CFG. This only happens for interprocedural layout and indicates
  // that a chain has nodes from different CFGs.
  if (cfg_ && cfg_ != unsplit_chain.cfg_) cfg_ = nullptr;
}

void NodeChain::MergeChainEdges(NodeChain &other) {
  // Helper function to transfer `edges` to edges from `from_chain` to
  // `to_chain`. If `&from_chain == &to_chain`, each CFGEdge will be moved to
  // `intra_chain_out_edges_` of its source bundle. Otherwise, they will be
  // moved to `from_chain.inter_chain_out_edges_` and
  // `to_chain.inter_chain_in_edges_`.
  auto move_edges = [](std::vector<CFGEdge *> edges, NodeChain &from_chain,
                       NodeChain &to_chain) {
    if (&from_chain == &to_chain) {
      for (CFGEdge *edge : edges)
        edge->src()->bundle()->intra_chain_out_edges_.push_back(edge);
    } else {
      auto [it, inserted] = from_chain.inter_chain_out_edges_.try_emplace(
          &to_chain, std::move(edges));
      if (inserted) {
        to_chain.inter_chain_in_edges_.insert(&from_chain);
      } else {
        // If the chain-edge is already present, just add the CFG edges.
        absl::c_move(std::move(edges), std::back_inserter(it->second));
      }
    }
  };

  // Move out-edges of the `other` `NodeChain` to out-edges of `*this`
  // `NodeChain`.
  for (auto &[chain, edges] : other.inter_chain_out_edges_) {
    CHECK_NE(chain, &other)
        << "Intra-chain edges found within inter-chain edges.";
    move_edges(std::move(edges), *this, *chain);
    chain->inter_chain_in_edges_.erase(&other);
  }

  // Move in-edges of the `other` `NodeChain` to in-edges of `*this` chain.
  for (auto *chain : other.inter_chain_in_edges_) {
    CHECK_NE(chain, &other)
        << "Intra-chain edges found within inter-chain edges.";
    auto edges_to_other = chain->inter_chain_out_edges_.find(&other);
    CHECK(edges_to_other != chain->inter_chain_out_edges_.end());
    move_edges(std::move(edges_to_other->second), *chain, *this);
    chain->inter_chain_out_edges_.erase(edges_to_other);
  }
}

void NodeChain::RemoveIntraBundleEdges(
    const PropellerCodeLayoutScorer &scorer) {
  for (std::unique_ptr<CFGNodeBundle> &bundle : node_bundles_) {
    auto it = std::remove_if(
        bundle->intra_chain_out_edges_.begin(),
        bundle->intra_chain_out_edges_.end(), [&](const CFGEdge *edge) {
          if (edge->src()->bundle() == edge->sink()->bundle()) {
            int64_t distance = edge->sink()->bundle_offset() -
                               edge->src()->bundle_offset() -
                               edge->src()->size();
            score_ -= scorer.GetEdgeScore(*edge, distance);
            return true;
          }
          return false;
        });
    bundle->intra_chain_out_edges_.erase(it,
                                         bundle->intra_chain_out_edges_.end());
  }
}

}  // namespace devtools_crosstool_autofdo
