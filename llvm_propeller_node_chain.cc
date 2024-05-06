#include "llvm_propeller_node_chain.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain_assembly.h"
#include "llvm_propeller_options.pb.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"

namespace devtools_crosstool_autofdo {

namespace {
// Returns whether merging chains in `assembly` requires rebundling their
// `node_bundles_`.
bool ShouldRebundle(const NodeChainAssembly &assembly,
                    const PropellerCodeLayoutParameters &code_layout_params) {
  return (!assembly.split_chain().function_index().has_value() ||
          (assembly.split_chain().function_index() !=
           assembly.unsplit_chain().function_index())) &&
         code_layout_params.chain_split() &&
         assembly.split_chain().node_bundles().size() +
                 assembly.unsplit_chain().node_bundles().size() >
             code_layout_params.chain_split_threshold();
}

std::pair<absl::flat_hash_map<int, int>, int> GetBaseOrdinalByFunctionIndex(
    const std::vector<const ControlFlowGraph *> &cfgs) {
  absl::flat_hash_map<int, int> base_index_by_function_index;
  int node_index = 0;
  // TODO(rahmanl) : Limit the index range to hot nodes.
  for (const ControlFlowGraph *cfg : cfgs) {
    base_index_by_function_index[cfg->function_index()] = node_index;
    node_index += cfg->nodes().size();
  }
  return std::make_pair(std::move(base_index_by_function_index), node_index);
}

// Specialized implementation of NodeToBundleMapper for intra-procedural
// reordering (single CFG).
class SingleCfgNodeToBundleMapper : public NodeToBundleMapper {
 public:
  // Constructs a `SingleCfgNodeToBundleMapper` to map nodes of a single CFG.
  explicit SingleCfgNodeToBundleMapper(const ControlFlowGraph &cfg)
      : NodeToBundleMapper(cfg.nodes().size()) {}

  int GetNodeIndex(const CFGNode *node) const override {
    return node->node_index();
  }
};

// Specialized implementation of NodeToBundleMapper for inter-procedural
// reordering (multiple cfgs).
class MultiCfgNodeToBundleMapper : public NodeToBundleMapper {
 public:
  explicit MultiCfgNodeToBundleMapper(
      absl::flat_hash_map<int, int> base_index_by_function_index,
      int ordinal_size)
      : NodeToBundleMapper(ordinal_size),
        base_index_by_function_index_(std::move(base_index_by_function_index)) {
  }

  int GetNodeIndex(const CFGNode *node) const override {
    return node->node_index() +
           base_index_by_function_index_.at(node->function_index());
  }

 private:
  // This map stores the base index for each function. The node indices of the
  // function with function_index I are within
  // [base_index_by_function_index_[I], base_index_by_function_index_[I+1]].
  const absl::flat_hash_map<int, int> base_index_by_function_index_;
};
}  // namespace

void NodeChain::MergeChainEdges(NodeChain &other,
                                const NodeToBundleMapper &bundle_mapper) {
  // Helper function to transfer `edges` to edges from `from_chain` to
  // `to_chain`. If `&from_chain == &to_chain`, each CFGEdge will be moved to
  // `intra_chain_out_edges_` of its source bundle. Otherwise, they will be
  // moved to `from_chain.inter_chain_out_edges_` and
  // `to_chain.inter_chain_in_edges_`.
  auto move_edges = [&](std::vector<const CFGEdge *> edges,
                        NodeChain &from_chain, NodeChain &to_chain) {
    if (&from_chain == &to_chain) {
      for (const CFGEdge *edge : edges) {
        DCHECK_NE(bundle_mapper.GetBundleMappingEntry(edge->src()).bundle,
                  bundle_mapper.GetBundleMappingEntry(edge->sink()).bundle);
        bundle_mapper.GetBundleMappingEntry(edge->src())
            .bundle->mutable_intra_chain_out_edges()
            .push_back(edge);
      }
      return;
    }

    auto [it, inserted] = from_chain.inter_chain_out_edges_.try_emplace(
        &to_chain, std::move(edges));
    if (inserted) {
      to_chain.inter_chain_in_edges_.insert(&from_chain);
    } else {
      // If the chain-edge is already present, just add the CFG edges.
      absl::c_move(std::move(edges), std::back_inserter(it->second));
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
    // We can't erase the iterator `edges_to_other` since absl::flat_hash_map
    // doesn't provide iterator stability.
    chain->inter_chain_out_edges_.erase(&other);
  }
}

void NodeChain::RemoveIntraBundleEdges(
    const NodeToBundleMapper &bundle_mapper,
    const PropellerCodeLayoutScorer &code_layout_scorer) {
  for (std::unique_ptr<CFGNodeBundle> &bundle : mutable_node_bundles()) {
    auto it = std::remove_if(
        bundle->mutable_intra_chain_out_edges().begin(),
        bundle->mutable_intra_chain_out_edges().end(),
        [&](const CFGEdge *edge) {
          const auto &src_bundle_info =
              bundle_mapper.GetBundleMappingEntry(edge->src());
          const auto &sink_bundle_info =
              bundle_mapper.GetBundleMappingEntry(edge->sink());
          if (src_bundle_info.bundle == sink_bundle_info.bundle) {
            int64_t distance = sink_bundle_info.bundle_offset -
                               src_bundle_info.bundle_offset -
                               edge->src()->size();
            score_ -= code_layout_scorer.GetEdgeScore(*edge, distance);
            return true;
          }
          return false;
        });
    bundle->mutable_intra_chain_out_edges().erase(
        it, bundle->mutable_intra_chain_out_edges().end());
  }
}

// Sorts `intra_chain_out_edges_` of every bundle in the chain. This should be
// called every time `intra_chain_out_edges_` is modified.
void NodeChain::SortIntraChainEdges(
    const NodeToBundleMapper &node_to_bundle_mapper) {
  for (std::unique_ptr<CFGNodeBundle> &bundle : mutable_node_bundles()) {
    // Sort edges based on the position of the sink node's bundle in the
    // chain.
    absl::c_sort(
        bundle->mutable_intra_chain_out_edges(),
        [&](const CFGEdge *e1, const CFGEdge *e2) {
          return std::forward_as_tuple(
                     node_to_bundle_mapper.GetBundleMappingEntry(e1->sink())
                         .bundle->chain_mapping()
                         .chain_index,
                     e1->src()->inter_cfg_id()) <
                 std::forward_as_tuple(
                     node_to_bundle_mapper.GetBundleMappingEntry(e2->sink())
                         .bundle->chain_mapping()
                         .chain_index,
                     e2->src()->inter_cfg_id());
        });
  }
}

void NodeChain::MergeWith(NodeChainAssembly assembly,
                          NodeToBundleMapper &node_to_bundle_mapper,
                          const PropellerCodeLayoutScorer &code_layout_scorer) {
  CHECK(this == &assembly.split_chain());
  NodeChain &unsplit_chain = assembly.unsplit_chain();

  // Merge in and out edges of the `unsplit_chain` into those of `*this`.
  MergeChainEdges(unsplit_chain, node_to_bundle_mapper);

  // Update the size of the aggregated chain.
  size_ += unsplit_chain.size();
  // Update the total frequency of the aggregated chain.
  freq_ += unsplit_chain.freq();

  // We have already computed the new score in the assembly record. So we can
  // update the score based on that and the other chain's score.
  score_ += unsplit_chain.score() + assembly.score_gain();

  // Actually merge the nodes of `unsplit_chain` into `*this`, in the order
  // given by `assembly`. `assembly` will be dead after the call to
  // `ConsumeEachNodeBundleInAssemblyOrder`.
  std::vector<std::unique_ptr<CFGNodeBundle>> merged_node_bundles;
  int chain_offset = 0;
  int chain_index = 0;
  CFGNodeBundle *prev_bundle = nullptr;
  bool should_rebundle =
      ShouldRebundle(assembly, code_layout_scorer.code_layout_params());
  int n_bundles_pre_merge = assembly.split_chain().node_bundles().size() +
                            assembly.unsplit_chain().node_bundles().size();
  std::move(assembly).ConsumeEachNodeBundleInAssemblyOrder(
      [&](std::unique_ptr<CFGNodeBundle> node_bundle) {
        // Try rebundling if this bundle comes from the same CFG as the previous
        // one.
        if (should_rebundle && prev_bundle != nullptr &&
            prev_bundle->nodes().front()->function_index() ==
                node_bundle->nodes().front()->function_index()) {
          chain_offset += node_bundle->size();
          for (const CFGNode *node : node_bundle->nodes()) {
            node_to_bundle_mapper.AdjustBundleMappingEntry(
                node,
                {.bundle = prev_bundle, .bundle_offset = prev_bundle->size()});
          }
          prev_bundle->MergeWith(*std::move(node_bundle));
        } else {
          node_bundle->SetChainMappingEntry({.chain = this,
                                             .chain_index = chain_index++,
                                             .chain_offset = chain_offset});
          chain_offset += node_bundle->size();
          prev_bundle = node_bundle.get();
          merged_node_bundles.push_back(std::move(node_bundle));
        }
      });

  node_bundles_ = std::move(merged_node_bundles);

  // Remove intra-bundle edges if we have actually rebundled any chain, i.e.,
  // if the `node_bundles().size()` has changed since before the merge.
  if (n_bundles_pre_merge != node_bundles().size())
    RemoveIntraBundleEdges(node_to_bundle_mapper, code_layout_scorer);
  SortIntraChainEdges(node_to_bundle_mapper);

  // Nullify function_index_ of the aggregated chain if the other chain comes
  // from a different CFG. This only happens for interprocedural layout and
  // indicates that a chain has nodes from different CFGs.
  if (function_index_ != unsplit_chain.function_index_) function_index_.reset();
}

double NodeChain::ComputeScore(const NodeToBundleMapper &bundle_mapper,
                               const PropellerCodeLayoutScorer &scorer) const {
  double score = 0;
  for (const std::unique_ptr<CFGNodeBundle> &bundle : node_bundles()) {
    for (const CFGEdge *edge : bundle->intra_chain_out_edges()) {
      int src_index = bundle_mapper.GetNodeIndex(edge->src());
      int sink_index = bundle_mapper.GetNodeIndex(edge->sink());
      CHECK_NE(bundle_mapper.GetBundleMappingEntry(sink_index).bundle,
               bundle.get());
      int src_offset = bundle_mapper.GetNodeOffset(src_index);
      int sink_offset = bundle_mapper.GetNodeOffset(sink_index);
      score += scorer.GetEdgeScore(
          *edge, sink_offset - src_offset - edge->src()->size());
    }
  }
  return score;
}

std::unique_ptr<NodeToBundleMapper>
NodeToBundleMapper::CreateNodeToBundleMapper(
    const std::vector<const ControlFlowGraph *> &cfgs) {
  if (cfgs.size() == 1)
    return std::make_unique<SingleCfgNodeToBundleMapper>(*cfgs.front());
  auto [base_index_by_function_index, ordinal_size] =
      GetBaseOrdinalByFunctionIndex(cfgs);
  return std::make_unique<MultiCfgNodeToBundleMapper>(
      std::move(base_index_by_function_index), ordinal_size);
}
}  // namespace devtools_crosstool_autofdo
