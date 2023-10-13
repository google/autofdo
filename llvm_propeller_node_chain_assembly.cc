#include "llvm_propeller_node_chain_assembly.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"

namespace devtools_crosstool_autofdo {

absl::string_view GetMergeOrderName(MergeOrder merge_order) {
  switch (merge_order) {
    case MergeOrder::kSU:
      return "SU";
    case MergeOrder::kS2S1U:
      return "S2S1U";
    case MergeOrder::kS1US2:
      return "S1US2";
    case MergeOrder::kUS2S1:
      return "US2S1";
    case MergeOrder::kS2US1:
      return "S2US1";
  }
  LOG(FATAL) << "invalid merge order.";
}

absl::StatusOr<NodeChainAssembly> NodeChainAssembly::BuildNodeChainAssembly(
    const PropellerCodeLayoutScorer &scorer, NodeChain &split_chain,
    NodeChain &unsplit_chain, NodeChainAssemblyBuildingOptions options) {
  CHECK_NE(split_chain.id(), unsplit_chain.id())
      << "Cannot construct an assembly between a chain and itself.";
  if (options.merge_order == MergeOrder::kSU) {
    CHECK(!options.slice_pos.has_value())
        << "slice_pos must not be provided for kSU merge order.";
  } else {
    CHECK(options.slice_pos.has_value())
        << "slice_pos is required for every merge order other than kSU.";
    CHECK_LT(*options.slice_pos, split_chain.node_bundles_.size())
        << "Out of bounds slice position.";
    CHECK_GT(*options.slice_pos, 0) << "Out of bounds slice position.";
  }
  NodeChainAssembly assembly(scorer, split_chain, unsplit_chain,
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
        "Assembly has negative score gain: %lld", assembly.score_gain()));
  } else if (assembly.score_gain() == 0 && options.error_on_zero_score_gain) {
    return absl::FailedPreconditionError("Assembly has zero score gain.");
  }
  return assembly;
}

int64_t NodeChainAssembly::ComputeScoreGain(
    const PropellerCodeLayoutScorer &scorer) const {
  // First compute the inter-chain score.
  int64_t score_gain =
      ComputeInterChainScore(scorer, split_chain(), unsplit_chain()) +
      ComputeInterChainScore(scorer, unsplit_chain(), split_chain());
  // As an optimization, if the inter-chain score gain is zero, we omit the
  // exact computation of the score gain and simply return 0.
  if (score_gain == 0) return 0;
  // Consider the change in score from split_chain as well.
  return score_gain + ComputeSplitChainScoreGain(scorer);
}

std::vector<NodeChainSlice> NodeChainAssembly::ConstructSlices() const {
  NodeChainSlice unsplit(unsplit_chain());
  if (merge_order_ == MergeOrder::kSU)
    return {NodeChainSlice(split_chain()), unsplit};

  NodeChainSlice split1(split_chain(), 0, *slice_pos_);
  NodeChainSlice split2(split_chain(), *slice_pos_,
                    split_chain().node_bundles_.size());
  switch (merge_order_) {
    case MergeOrder::kSU:
      LOG(FATAL) << "Unreachable.";
    case MergeOrder::kS2S1U:
      return {split2, split1, unsplit};
    case MergeOrder::kS1US2:
      return {split1, unsplit, split2};
    case MergeOrder::kUS2S1:
      return {unsplit, split2, split1};
    case MergeOrder::kS2US1:
      return {split2, unsplit, split1};
  }
  LOG(FATAL) << "Invalid merge order.";
}

std::optional<int> NodeChainAssembly::FindSliceIndex(
    const CFGNode *node) const {
  const NodeChain &chain = GetNodeChain(node);
  if (chain.id() == unsplit_chain().id()) return unsplit_chain_slice_index();
  if (chain.id() != split_chain().id()) return std::nullopt;
  // If this is not a splitting assembly, it will have the SU merge order.
  // So the slice index will be 0.
  if (!splits()) return 0;
  const int64_t offset = GetNodeOffset(node);
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
        for (auto node_it = (*node_bundle_it)->nodes_.rbegin();
             node_it != (*node_bundle_it)->nodes_.rend(); ++node_it) {
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
        for (auto node_it = (*node_bundle_it)->nodes_.begin();
             node_it != (*node_bundle_it)->nodes_.end(); ++node_it) {
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
int64_t NodeChainAssembly::ComputeEdgeScore(
    const PropellerCodeLayoutScorer &scorer, const CFGEdge &edge) const {
  const int src_slice_idx = FindSliceIndex(edge.src()).value();
  const int sink_slice_idx = FindSliceIndex(edge.sink()).value();

  const int64_t src_node_offset = GetNodeOffset(edge.src());
  const int64_t sink_node_offset = GetNodeOffset(edge.sink());
  int64_t src_sink_distance = 0;

  if (src_slice_idx == sink_slice_idx) {
    src_sink_distance = sink_node_offset - src_node_offset - edge.src()->size();
  } else {
    bool edge_forward = src_slice_idx < sink_slice_idx;
    const NodeChainSlice &src_slice = slices_[src_slice_idx];
    const NodeChainSlice &sink_slice = slices_[sink_slice_idx];
    src_sink_distance =
        edge_forward
            ? src_slice.end_offset() - src_node_offset - edge.src()->size() +
                  sink_node_offset - sink_slice.begin_offset()
            : src_slice.begin_offset() - src_node_offset - edge.src()->size() +
                  sink_node_offset - sink_slice.end_offset();
    // Increment the distance by the size of the middle slice if the src
    // and sink are from the two ends.
    if (src_slice_idx == 0 && sink_slice_idx == 2)
      src_sink_distance += slices_[1].size();
    else if (src_slice_idx == 2 && sink_slice_idx == 0)
      src_sink_distance -= slices_[1].size();
  }
  return scorer.GetEdgeScore(edge, src_sink_distance);
}

int64_t NodeChainAssembly::ComputeInterChainScore(
    const PropellerCodeLayoutScorer &scorer, NodeChain &from_chain,
    NodeChain &to_chain) const {
  auto it = from_chain.inter_chain_out_edges_.find(&to_chain);
  if (it == from_chain.inter_chain_out_edges_.end()) return 0;
  int64_t score = 0;
  for (const CFGEdge *edge : it->second)
    score += ComputeEdgeScore(scorer, *edge);
  return score;
}

// Returns the score gain from intra-chain edges of `split_chain()` for this
// assembly. Effectively, we aggregate the score difference of inter-slice
// edges, i.e., edges from one slice of `split_chain()` to the other. This is
// correct because intra-slice edges will see no difference in score.
int64_t NodeChainAssembly::ComputeSplitChainScoreGain(
    const PropellerCodeLayoutScorer &scorer) const {
  if (!splits()) return 0;
  int64_t score_gain = 0;
  auto get_score_gain = [&](const CFGEdge &edge) {
    return ComputeEdgeScore(scorer, edge) -
           scorer.GetEdgeScore(edge, GetNodeOffset(edge.sink()) -
                                         GetNodeOffset(edge.src()) -
                                         edge.src()->size());
  };
  // Visit edges from the first slice (before `slice_pos_`) to the second slice.
  for (int i = 0; i < *slice_pos_; ++i) {
    const auto &bundle = split_chain().node_bundles_[i];
    for (auto it = bundle->intra_chain_out_edges_.rbegin(),
              it_end = bundle->intra_chain_out_edges_.rend();
         it != it_end && (*it)->sink()->bundle()->chain_index_ >= *slice_pos_;
         ++it) {
      score_gain += get_score_gain(**it);
    }
  }
  // Visit edges from the second slice (on and after `slice_pos_`) to the first
  // slice.
  for (int i = *slice_pos_; i < split_chain().node_bundles_.size(); ++i) {
    const auto &bundle = split_chain().node_bundles_[i];
    for (auto it = bundle->intra_chain_out_edges_.begin(),
              it_end = bundle->intra_chain_out_edges_.end();
         it != it_end && (*it)->sink()->bundle()->chain_index_ < *slice_pos_;
         ++it) {
      score_gain += get_score_gain(**it);
    }
  }
  return score_gain;
}

bool NodeChainAssembly::NodeChainAssemblyComparator::operator()(
    const NodeChainAssembly &lhs, const NodeChainAssembly &rhs) const {
  if (lhs.score_gain() != rhs.score_gain())
    return lhs.score_gain() < rhs.score_gain();

  // Tie-breaking for when score gains are equal:
  // Edges among basic blocks with lower indices are ranked higher.
  NodeChain::RefComparator comp;
  if (comp(lhs.split_chain(), rhs.split_chain())) return false;
  if (comp(rhs.split_chain(), lhs.split_chain())) return true;
  if (comp(lhs.unsplit_chain(), rhs.unsplit_chain())) return false;
  if (comp(rhs.unsplit_chain(), lhs.unsplit_chain())) return true;

  // When even the chain pairs are the same, we resort to the assembly
  // strategy (merge_order and slice_pos) to pick a consistent order.
  if (lhs.merge_order() == rhs.merge_order())
    return *lhs.slice_pos() < *rhs.slice_pos();
  return lhs.merge_order() < rhs.merge_order();
}

}  // namespace devtools_crosstool_autofdo
