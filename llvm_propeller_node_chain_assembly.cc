#include "llvm_propeller_node_chain_assembly.h"

#include <iterator>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"

namespace devtools_crosstool_autofdo {

absl::StatusOr<NodeChainAssembly> NodeChainAssembly::BuildNodeChainAssembly(
    const NodeToBundleMapper &bundle_mapper,
    const PropellerCodeLayoutScorer &scorer, NodeChain &split_chain,
    NodeChain &unsplit_chain, NodeChainAssemblyBuildingOptions options) {
  CHECK_NE(split_chain.id(), unsplit_chain.id())
      << "Cannot construct an assembly between a chain and itself.";
  if (options.merge_order == ChainMergeOrder::kSU) {
    CHECK(!options.slice_pos.has_value())
        << "slice_pos must not be provided for kSU merge order.";
  } else {
    CHECK(options.slice_pos.has_value())
        << "slice_pos is required for every merge order other than kSU.";
    CHECK_LT(*options.slice_pos, split_chain.node_bundles().size())
        << "Out of bounds slice position.";
    CHECK_GT(*options.slice_pos, 0) << "Out of bounds slice position.";
  }
  NodeChainAssembly assembly(bundle_mapper, scorer, split_chain, unsplit_chain,
                             options.merge_order, options.slice_pos);
  // If `inter_function_ordering = false`, omit assemblies which place the entry
  // node in the middle of the chain. Placing the entry block in the middle is
  // allowed. However, it requires multiple hot function parts (sections) as the
  // function entry always marks the beginning of a section.
  if (!scorer.code_layout_params().inter_function_reordering() &&
      (split_chain.GetFirstNode()->is_entry() ||
       unsplit_chain.GetFirstNode()->is_entry()) &&
      !assembly.GetFirstNode()->is_entry()) {
    return absl::FailedPreconditionError(
        "Assembly places the entry block in the middle.");
  }

  // Also omit assemblies without positive gain.
  if (assembly.score_gain() < 0) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "Assembly has negative score gain: %f", assembly.score_gain()));
  } else if (assembly.score_gain() == 0 && options.error_on_zero_score_gain) {
    return absl::FailedPreconditionError("Assembly has zero score gain.");
  }
  return assembly;
}

double NodeChainAssembly::ComputeScoreGain(
    const NodeToBundleMapper &bundle_mapper,
    const PropellerCodeLayoutScorer &scorer) const {
  // First compute the inter-chain score.
  double score_gain = ComputeInterChainScore(bundle_mapper, scorer,
                                             split_chain(), unsplit_chain()) +
                      ComputeInterChainScore(bundle_mapper, scorer,
                                             unsplit_chain(), split_chain());
  // As an optimization, if the inter-chain score gain is zero, we omit the
  // exact computation of the score gain and simply return 0.
  if (score_gain == 0) return 0;
  // Consider the change in score from split_chain as well.
  return score_gain + ComputeSplitChainScoreGain(bundle_mapper, scorer);
}

std::vector<NodeChainSlice> NodeChainAssembly::ConstructSlices() const {
  NodeChainSlice unsplit(unsplit_chain());
  if (merge_order_ == ChainMergeOrder::kSU)
    return {NodeChainSlice(split_chain()), unsplit};

  NodeChainSlice split1(split_chain(), 0, *slice_pos_);
  NodeChainSlice split2(split_chain(), *slice_pos_,
                        split_chain().node_bundles().size());
  switch (merge_order_) {
    case ChainMergeOrder::kSU:
      LOG(FATAL) << "Unreachable.";
    case ChainMergeOrder::kS2S1U:
      return {split2, split1, unsplit};
    case ChainMergeOrder::kS1US2:
      return {split1, unsplit, split2};
    case ChainMergeOrder::kUS2S1:
      return {unsplit, split2, split1};
    case ChainMergeOrder::kS2US1:
      return {split2, unsplit, split1};
  }
  LOG(FATAL) << "Invalid merge order.";
}

std::optional<int> NodeChainAssembly::FindSliceIndex(
    const CFGNode *node,
    const NodeToBundleMapper::BundleMappingEntry &bundle_mapping) const {
  int offset = bundle_mapping.GetNodeOffset();
  const NodeChain &chain = *bundle_mapping.bundle->chain_mapping().chain;
  if (chain.id() == unsplit_chain().id()) return unsplit_chain_slice_index();
  if (chain.id() != split_chain().id()) return std::nullopt;
  // If this is not a splitting assembly, it will have the SU merge order.
  // So the slice index will be 0.
  if (!splits()) return 0;
  for (int idx : split_chain_slice_indexes()) {
    CHECK_EQ(chain.id(), slices()[idx].chain().id());
    if (offset < slices()[idx].end_offset() &&
        offset > slices()[idx].begin_offset()) {
      return idx;
    }
    // A node can have zero size, which means multiple nodes may be associated
    // with the same offset. This means that if the node's offset is at the
    // beginning or the end of the slice, the node may reside in either
    // slices_ of the chain.
    if (offset == slices()[idx].end_offset()) {
      // If offset is at the end of the slice, iterate backwards over the
      // slice to find the node among the zero-sized nodes at the end of the
      // slice.
      for (auto node_bundle_it =
               std::make_reverse_iterator(slices()[idx].end_pos());
           node_bundle_it !=
           std::make_reverse_iterator(slices()[idx].begin_pos());
           ++node_bundle_it) {
        for (auto node_it = (*node_bundle_it)->nodes().rbegin();
             node_it != (*node_bundle_it)->nodes().rend(); ++node_it) {
          // Stop iterating if the node's size is non-zero as this would
          // change the offset.
          if ((*node_it)->size() != 0) break;
          if (*node_it == node) return idx;
        }
      }
    }
    if (offset == slices_[idx].begin_offset()) {
      // If offset is at the beginning of the slice, iterate forwards over the
      // slice to find the node among the zero-sized nodes at the beginning of
      // the slice.
      for (auto node_bundle_it = slices()[idx].begin_pos();
           node_bundle_it != slices()[idx].end_pos(); ++node_bundle_it) {
        for (auto node_it = (*node_bundle_it)->nodes().begin();
             node_it != (*node_bundle_it)->nodes().end(); ++node_it) {
          if (*node_it == node) return idx;
          // Stop iterating if the node's size is non-zero as this would
          // change the offset.
          if ((*node_it)->size() != 0) break;
        }
      }
    }
  }
  return std::nullopt;
}

// Returns the score contribution of a single edge for this chain assembly.
double NodeChainAssembly::ComputeEdgeScore(
    const NodeToBundleMapper &bundle_mapper,
    const PropellerCodeLayoutScorer &scorer, const CFGEdge &edge) const {
  const auto &src_bundle_info = bundle_mapper.GetBundleMappingEntry(edge.src());
  const auto &sink_bundle_info =
      bundle_mapper.GetBundleMappingEntry(edge.sink());

  const int src_slice_idx = FindSliceIndex(edge.src(), src_bundle_info).value();
  const int sink_slice_idx =
      FindSliceIndex(edge.sink(), sink_bundle_info).value();

  int src_sink_distance = 0;
  int src_offset = src_bundle_info.GetNodeOffset();
  int sink_offset = sink_bundle_info.GetNodeOffset();

  if (src_slice_idx == sink_slice_idx) {
    src_sink_distance = sink_offset - src_offset - edge.src()->size();
  } else {
    bool edge_forward = src_slice_idx < sink_slice_idx;
    const NodeChainSlice &src_slice = slices_[src_slice_idx];
    const NodeChainSlice &sink_slice = slices_[sink_slice_idx];
    src_sink_distance =
        edge_forward
            ? src_slice.end_offset() - src_offset - edge.src()->size() +
                  sink_offset - sink_slice.begin_offset()
            : src_slice.begin_offset() - src_offset - edge.src()->size() +
                  sink_offset - sink_slice.end_offset();
    // Increment the distance by the size of the middle slice if the src
    // and sink are from the two ends.
    if (src_slice_idx == 0 && sink_slice_idx == 2)
      src_sink_distance += slices_[1].size();
    else if (src_slice_idx == 2 && sink_slice_idx == 0)
      src_sink_distance -= slices_[1].size();
  }
  return scorer.GetEdgeScore(edge, src_sink_distance);
}

double NodeChainAssembly::ComputeInterChainScore(
    const NodeToBundleMapper &bundle_mapper,
    const PropellerCodeLayoutScorer &scorer, const NodeChain &from_chain,
    const NodeChain &to_chain) const {
  auto it = from_chain.inter_chain_out_edges().find(&to_chain);
  if (it == from_chain.inter_chain_out_edges().end()) return 0;
  double score = 0;
  for (const CFGEdge *edge : it->second)
    score += ComputeEdgeScore(bundle_mapper, scorer, *edge);
  return score;
}

// Returns the score gain from intra-chain edges of `split_chain()` for this
// assembly. Effectively, we aggregate the score difference of inter-slice
// edges, i.e., edges from one slice of `split_chain()` to the other. This is
// correct because intra-slice edges will see no difference in score.
double NodeChainAssembly::ComputeSplitChainScoreGain(
    const NodeToBundleMapper &bundle_mapper,
    const PropellerCodeLayoutScorer &scorer) const {
  if (!splits()) return 0;
  double score_gain = 0;
  auto get_score_gain = [&](const CFGEdge &edge) {
    return ComputeEdgeScore(bundle_mapper, scorer, edge) -
           scorer.GetEdgeScore(edge,
                               bundle_mapper.GetNodeOffset(edge.sink()) -
                                   bundle_mapper.GetNodeOffset(edge.src()) -
                                   edge.src()->size());
  };
  // Visit edges from the first slice (before `slice_pos_`) to the second slice.
  for (int i = 0; i < *slice_pos_; ++i) {
    const auto &bundle = split_chain().node_bundles()[i];
    for (auto it = bundle->intra_chain_out_edges().rbegin(),
              it_end = bundle->intra_chain_out_edges().rend();
         it != it_end && bundle_mapper.GetBundleMappingEntry((*it)->sink())
                                 .bundle->chain_mapping()
                                 .chain_index >= *slice_pos_;
         ++it) {
      score_gain += get_score_gain(**it);
    }
  }
  // Visit edges from the second slice (on and after `slice_pos_`) to the first
  // slice.
  for (int i = *slice_pos_; i < split_chain().node_bundles().size(); ++i) {
    const auto &bundle = split_chain().node_bundles()[i];
    for (auto it = bundle->intra_chain_out_edges().begin(),
              it_end = bundle->intra_chain_out_edges().end();
         it != it_end && bundle_mapper.GetBundleMappingEntry((*it)->sink())
                                 .bundle->chain_mapping()
                                 .chain_index < *slice_pos_;
         ++it) {
      score_gain += get_score_gain(**it);
    }
  }
  return score_gain;
}

// Comparator for NodeChainAssemblies based on score gain, with tie-breaking for
// when score gains are equal:
//  Edges among basic blocks with lower indices are ranked higher. Finally, we
//  resort to the merge order and slice position for complete tie-breaking.
bool NodeChainAssembly::NodeChainAssemblyComparator::operator()(
    const NodeChainAssembly &lhs, const NodeChainAssembly &rhs) const {
  return std::make_tuple(lhs.score_gain(), rhs.split_chain().id(),
                         rhs.unsplit_chain().id(), lhs.merge_order(),
                         lhs.slice_pos()) <
         std::make_tuple(rhs.score_gain(), lhs.split_chain().id(),
                         lhs.unsplit_chain().id(), rhs.merge_order(),
                         rhs.slice_pos());
}

}  // namespace devtools_crosstool_autofdo
