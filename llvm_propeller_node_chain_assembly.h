#ifndef AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_ASSEMBLY_H_
#define AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_ASSEMBLY_H_

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {

// This struct defines a slices of a node chain, specified by iterators to the
// beginning and end of the slice.
class NodeChainSlice {
 public:
  // Constructor for building a chain slice from a given chain and the two
  // endpoints of the chain. `chain` must outlive the NodeChainSlice.
  // Additionally, any changes to the bundles of the chain would invalidate the
  // slice.
  explicit NodeChainSlice(NodeChain &chain, int begin, int end)
      : chain_(&chain),
        begin_index_(begin),
        end_index_(end) {
    CHECK_LE(begin, end);
    CHECK_LE(begin, chain.node_bundles_.size());
    CHECK_LE(end, chain.node_bundles_.size());
  }

  // Constructor for building a chain slice from a node chain containing all
  // of its nodes.
  explicit NodeChainSlice(NodeChain &chain)
      : chain_(&chain),
        begin_index_(0),
        end_index_(chain.node_bundles_.size()) {}

  // NodeChainSlice is copyable.
  NodeChainSlice(const NodeChainSlice&) = default;
  NodeChainSlice& operator=(const NodeChainSlice&) = default;
  NodeChainSlice(NodeChainSlice&&) = default;
  NodeChainSlice& operator=(NodeChainSlice&&) = default;

  NodeChain &chain() const { return *chain_; }
  // Iterator to the beginning of the slice.
  std::vector<std::unique_ptr<CFGNodeBundle>>::iterator begin_pos() const {
    return chain_->node_bundles_.begin() + begin_index_;
  }

  // Iterator to the end of the slice.
  std::vector<std::unique_ptr<CFGNodeBundle>>::iterator end_pos() const {
    return chain_->node_bundles_.begin() + end_index_;
  }

  // The binary-size offsets corresponding to the two end-points of the slice.
  int64_t begin_offset() const { return (*begin_pos())->chain_offset_; }
  int64_t end_offset() const {
    return end_index_ == chain_->node_bundles_.size() ? chain_->size_
                                                 : (*end_pos())->chain_offset_;
  }

  // (Binary) size of this slice
  int64_t size() const { return end_offset() - begin_offset(); }

  bool empty() const { return begin_index_ == end_index_; }

 private:
  // The chain from which this slice has been constructed.
  NodeChain *chain_;

  // The endpoints of the slice in the corresponding chain. `begin_index_` is
  // the first index included in the slice. `end_index_` is one after the last
  // included index.
  int begin_index_, end_index_;
};

// This enum represents different merge orders for two chains S and U.
// S1 and S2 are the slices of the S chain when it is split. S2US1 is ignored
// since it is rarely beneficial.
enum class MergeOrder {
  kSU,
  kS2S1U,
  kS1US2,
  kUS2S1,
  kS2US1,
};

absl::string_view GetMergeOrderName(MergeOrder merge_order);

// This class abstracts the strategy for assembling two chains together with one
// of the chains potentially being split into two chains. This strategy is
// specified by the following fields:
// 1- The two chains (split_chain and unsplit_chain),
// 2- The split position in the split_chain (if split_chain must be split).
// 3- The merge order for concatenating the three/two resulting chain slices.
//
// For ease of implementation, we form two or three 'NodeChainSlice's from these
// fields and use them to query and traverse the NodeChainAssembly.
class NodeChainAssembly {
 public:
  // Comparator for two NodeChainAssemblies. It compares score_gain and break
  // ties consistently.
  struct NodeChainAssemblyComparator {
    bool operator()(const NodeChainAssembly &lhs,
                    const NodeChainAssembly &rhs) const;
  };

  // Builds a NodeChainAssembly which merges `split_chain` and `unsplit_chain`
  // according to `merge_order`. Both chains must outlive the created
  // NodeChainAssembly. `slice_pos` must be std::nullopt iff `merge_order` is
  // kSU. Returns error under either of two conditions:
  // 1- The assembly places a function entry node in the middle.
  // 2- The constructed assembly has a non-positive score-gain.
  static absl::StatusOr<NodeChainAssembly> BuildNodeChainAssembly(
      const PropellerCodeLayoutScorer &scorer, NodeChain &split_chain,
      NodeChain &unsplit_chain, MergeOrder merge_order,
      std::optional<int> slice_pos = std::nullopt);

  // NodeChainAssembly is copyable and moveable.
  NodeChainAssembly(const NodeChainAssembly&) = default;
  NodeChainAssembly& operator=(const NodeChainAssembly&) = default;
  NodeChainAssembly(NodeChainAssembly&&) = default;
  NodeChainAssembly& operator=(NodeChainAssembly&&) = default;

  MergeOrder merge_order() const { return merge_order_; }

  std::optional<int> slice_pos() const { return slice_pos_; }

  NodeChain &split_chain() const { return *split_chain_; }
  NodeChain &unsplit_chain() const { return *unsplit_chain_; }

  const std::vector<NodeChainSlice>& slices() const { return slices_; }

  // Returns whether this assembly actually splits the split_chain.
  bool splits() const { return merge_order_ != MergeOrder::kSU; }

  int64_t score_gain() const { return score_gain_; }

  // Iterates over all node bundles in the resulting assembled chain while
  // applying a given function to every node bundle.
  void VisitEachNodeBundleInAssemblyOrder(
      absl::FunctionRef<void(const CFGNodeBundle &bundle)> func) const {
    for (const NodeChainSlice &slice : slices_) {
      for (auto it = slice.begin_pos(); it != slice.end_pos(); ++it) func(**it);
    }
  }

  void ConsumeEachNodeBundleInAssemblyOrder(
      absl::FunctionRef<void(std::unique_ptr<CFGNodeBundle> bundle)> func) && {
    for (const NodeChainSlice &slice : slices_) {
      for (auto it = slice.begin_pos(); it != slice.end_pos(); ++it)
        func(std::move(*it));
    }
  }

  // Gets the first node in the resulting assembled chain.
  CFGNode *GetFirstNode() const {
    return (*slices_.front().begin_pos())->nodes_.front();
  }

  // Finds the NodeChainSlice in this NodeChainAssembly which contains the given
  // node. If the node is not contained in this NodeChainAssembly, then return
  // a std::nullopt. Otherwise, return the corresponding index for the slice.
  std::optional<int> FindSliceIndex(const CFGNode *node) const;

 private:
  explicit NodeChainAssembly(const PropellerCodeLayoutScorer &scorer,
                             NodeChain &split_chain, NodeChain &unsplit_chain,
                             MergeOrder merge_order,
                             std::optional<int> slice_pos)
      : split_chain_(&split_chain),
        unsplit_chain_(&unsplit_chain),
        merge_order_(merge_order),
        slice_pos_(slice_pos),
        slices_(ConstructSlices()),
        score_gain_(ComputeScoreGain(scorer)) {}

  // Index of the unsplit_chain in the slices_ vector.
  int unsplit_chain_slice_index() const {
    switch (merge_order_) {
      case MergeOrder::kSU:
        return 1;
      case MergeOrder::kS2S1U:
        return 2;
      case MergeOrder::kS1US2:
        return 1;
      case MergeOrder::kUS2S1:
        return 0;
      case MergeOrder::kS2US1:
        return 1;
    }
    LOG(FATAL) << "Invalid merge order.";
  }

  // Indices of the split_chain slices in the slices_ vector.
  std::vector<int> split_chain_slice_indexes() const {
    switch (merge_order_) {
      case MergeOrder::kSU:
        return {0};
      case MergeOrder::kS2S1U:
        return {1, 0};
      case MergeOrder::kS1US2:
        return {0, 2};
      case MergeOrder::kUS2S1:
        return {2, 1};
      case MergeOrder::kS2US1:
        return {0, 2};
    }
    LOG(FATAL) << "Invalid merge_order";
  }


  // Constructs and returns the node chain slices that are characterized by the
  // specified slice position and merge order.
  std::vector<NodeChainSlice> ConstructSlices() const;

  // Returns the gain in Ext-TSP score if this assembly is applied. May return 0
  // if the actual score gain is negative.
  int64_t ComputeScoreGain(const PropellerCodeLayoutScorer &scorer) const;

  // Returns the total score contribution of edges running from `from_chain` to
  // `to_chain` for this assembly.
  int64_t ComputeInterChainScore(const PropellerCodeLayoutScorer &scorer,
                                 NodeChain &from_chain,
                                 NodeChain &to_chain) const;

  // Returns the score contribution of a single edge for this assembly.
  int64_t ComputeEdgeScore(const PropellerCodeLayoutScorer &scorer,
                            const CFGEdge &edge) const;

  // The two chains in the assembly. split_chain_ is the one that may be split.
  NodeChain *split_chain_;
  NodeChain *unsplit_chain_;

  // The merge order of the slices
  MergeOrder merge_order_;

  // The splitting position in split_chain; split_chain is split into
  // s1[0, slice_pos_ - 1] and s2[slice_pos_, split_chain.size()-1]
  // This will be empty for the kSU merge order.
  std::optional<int> slice_pos_;

  // The three chain slices formed from the fields above.
  std::vector<NodeChainSlice> slices_;

  // The gain in ExtTSP score achieved by this NodeChainAssembly once it
  // is accordingly applied to the two chains.
  // This is equal to
  // [assembled_chain]->score - split_chain->score - unsplit_chain->score".
  // This value is computed by calling ComputeScoreGain upon construction and is
  // cached here for efficiency.
  int64_t score_gain_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_ASSEMBLY_H_
