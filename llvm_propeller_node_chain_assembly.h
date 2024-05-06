#ifndef AUTOFDOLLVM_PROPELLER_NODE_CHAIN_ASSEMBLY_H_
#define AUTOFDOLLVM_PROPELLER_NODE_CHAIN_ASSEMBLY_H_

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_merge_order.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/status/statusor.h"

namespace devtools_crosstool_autofdo {

// Represents a pair of NodeChains <`split_chain`, `unsplit_chain`> associated
// with a `NodeChainAssembly`.
struct NodeChainPair {
  NodeChain *split_chain = nullptr;
  NodeChain *unsplit_chain = nullptr;
  friend bool operator==(const NodeChainPair &lhs, const NodeChainPair &rhs);
  template <typename H>
  friend H AbslHashValue(H h, const NodeChainPair &m);
};

inline bool operator==(const NodeChainPair &lhs, const NodeChainPair &rhs) {
  return lhs.split_chain == rhs.split_chain &&
         lhs.unsplit_chain == rhs.unsplit_chain;
}

template <typename H>
H AbslHashValue(H h, const NodeChainPair &m) {
  return H::combine(std::move(h), m.split_chain, m.unsplit_chain);
}

// This struct defines a slices of a node chain, specified by iterators to the
// beginning and end of the slice.
class NodeChainSlice {
 public:
  // Constructor for building a chain slice from a given chain and the two
  // endpoints of the chain. `chain` must outlive the NodeChainSlice.
  // Additionally, any changes to the bundles of the chain would invalidate the
  // slice.
  explicit NodeChainSlice(NodeChain &chain, int begin, int end)
      : chain_(&chain), begin_index_(begin), end_index_(end) {
    CHECK_LE(begin, end);
    CHECK_LE(begin, chain.node_bundles().size());
    CHECK_LE(end, chain.node_bundles().size());
  }

  // Constructor for building a chain slice from a node chain containing all
  // of its nodes.
  explicit NodeChainSlice(NodeChain &chain)
      : chain_(&chain),
        begin_index_(0),
        end_index_(chain.node_bundles().size()) {}

  // NodeChainSlice is copyable.
  NodeChainSlice(const NodeChainSlice &) = default;
  NodeChainSlice &operator=(const NodeChainSlice &) = default;
  NodeChainSlice(NodeChainSlice &&) = default;
  NodeChainSlice &operator=(NodeChainSlice &&) = default;

  NodeChain &chain() const { return *chain_; }
  // Iterator to the beginning of the slice.
  std::vector<std::unique_ptr<CFGNodeBundle>>::iterator begin_pos() const {
    return chain_->mutable_node_bundles().begin() + begin_index_;
  }

  // Iterator to the end of the slice.
  std::vector<std::unique_ptr<CFGNodeBundle>>::iterator end_pos() const {
    return chain_->mutable_node_bundles().begin() + end_index_;
  }

  // The binary-size offsets corresponding to the two end-points of the slice.
  int begin_offset() const {
    return (*begin_pos())->chain_mapping().chain_offset;
  }
  int end_offset() const {
    return end_index_ == chain_->node_bundles().size()
               ? chain_->size()
               : (*end_pos())->chain_mapping().chain_offset;
  }

  // (Binary) size of this slice
  int size() const { return end_offset() - begin_offset(); }

  bool empty() const { return begin_index_ == end_index_; }

 private:
  // The chain from which this slice has been constructed.
  NodeChain *chain_;

  // The endpoints of the slice in the corresponding chain. `begin_index_` is
  // the first index included in the slice. `end_index_` is one after the last
  // included index.
  int begin_index_, end_index_;
};

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
  // This struct represents options for building a `NodeChainAssembly` by
  // `NodeChainAssembly::BuildNodeChainAssembly`.
  struct NodeChainAssemblyBuildingOptions {
    // The merge order for concatenating the three/two resulting chain slices.
    ChainMergeOrder merge_order = ChainMergeOrder::kSU;
    // The split position in the split_chain (if split_chain must be split).
    std::optional<int> slice_pos = std::nullopt;
    // Whether `NodeChainAssembly::BuildNodeChainAssembly` should return error
    // if the constructed assembly's score gain is zero.
    bool error_on_zero_score_gain = true;
  };

  // Comparator for two NodeChainAssemblies. It compares score_gain and break
  // ties consistently.
  struct NodeChainAssemblyComparator {
    bool operator()(const NodeChainAssembly &lhs,
                    const NodeChainAssembly &rhs) const;
  };

  // Builds a NodeChainAssembly which merges `split_chain` and `unsplit_chain`
  // according to `options`. Both chains must outlive the created
  // NodeChainAssembly. `options.slice_pos` must be `std::nullopt` iff
  // `options.merge_order == kSU`. Returns error under these conditions:
  // 1- The assembly places a function entry node in the middle (in
  //    non-inter-function-reordering mode).
  // 2- `options.slice_pos` is out of bounds (less than 0 or larger than
  //    `split_chain.node_bundles.size() - 1`).
  // 3- The constructed assembly has a negative score gain or it has zero
  //    score gain and `options.error_on_zero_score_gain == true`.
  static absl::StatusOr<NodeChainAssembly> BuildNodeChainAssembly(
      const NodeToBundleMapper &bundle_mapper,
      const PropellerCodeLayoutScorer &scorer, NodeChain &split_chain,
      NodeChain &unsplit_chain, NodeChainAssemblyBuildingOptions options);

  // NodeChainAssembly is copyable and moveable.
  NodeChainAssembly(const NodeChainAssembly &) = default;
  NodeChainAssembly &operator=(const NodeChainAssembly &) = default;
  NodeChainAssembly(NodeChainAssembly &&) = default;
  NodeChainAssembly &operator=(NodeChainAssembly &&) = default;

  ChainMergeOrder merge_order() const { return merge_order_; }

  std::optional<int> slice_pos() const { return slice_pos_; }

  NodeChainPair chain_pair() const { return chain_pair_; }
  NodeChain &split_chain() const { return *chain_pair_.split_chain; }
  NodeChain &unsplit_chain() const { return *chain_pair_.unsplit_chain; }

  const std::vector<NodeChainSlice> &slices() const { return slices_; }

  // Returns whether this assembly actually splits the split_chain.
  bool splits() const { return merge_order_ != ChainMergeOrder::kSU; }

  double score_gain() const { return score_gain_; }

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
  const CFGNode *GetFirstNode() const {
    return (*slices_.front().begin_pos())->nodes().front();
  }

  // Finds the NodeChainSlice in this NodeChainAssembly which contains the given
  // node. If the node is not contained in this NodeChainAssembly, then return
  // a std::nullopt. Otherwise, return the corresponding index for the slice.
  std::optional<int> FindSliceIndex(
      const CFGNode *node,
      const NodeToBundleMapper::BundleMappingEntry &bundle_mapping) const;

 private:
  explicit NodeChainAssembly(const NodeToBundleMapper &bundle_mapper,
                             const PropellerCodeLayoutScorer &scorer,
                             NodeChain &split_chain, NodeChain &unsplit_chain,
                             ChainMergeOrder merge_order,
                             std::optional<int> slice_pos)
      : chain_pair_{.split_chain = &split_chain,
                    .unsplit_chain = &unsplit_chain},
        merge_order_(merge_order),
        slice_pos_(slice_pos),
        slices_(ConstructSlices()),
        score_gain_(ComputeScoreGain(bundle_mapper, scorer)) {}

  // Index of the unsplit_chain in the slices_ vector.
  int unsplit_chain_slice_index() const {
    switch (merge_order_) {
      case ChainMergeOrder::kSU:
        return 1;
      case ChainMergeOrder::kS2S1U:
        return 2;
      case ChainMergeOrder::kS1US2:
        return 1;
      case ChainMergeOrder::kUS2S1:
        return 0;
      case ChainMergeOrder::kS2US1:
        return 1;
    }
    LOG(FATAL) << "Invalid merge order.";
  }

  // Indices of the split_chain slices in the slices_ vector.
  std::vector<int> split_chain_slice_indexes() const {
    switch (merge_order_) {
      case ChainMergeOrder::kSU:
        return {0};
      case ChainMergeOrder::kS2S1U:
        return {1, 0};
      case ChainMergeOrder::kS1US2:
        return {0, 2};
      case ChainMergeOrder::kUS2S1:
        return {2, 1};
      case ChainMergeOrder::kS2US1:
        return {0, 2};
    }
    LOG(FATAL) << "Invalid merge_order";
  }

  // Constructs and returns the node chain slices that are characterized by the
  // specified slice position and merge order.
  std::vector<NodeChainSlice> ConstructSlices() const;

  // Returns the gain in Ext-TSP score if this assembly is applied. May return 0
  // if the actual score gain is negative.
  double ComputeScoreGain(const NodeToBundleMapper &bundle_mapper,
                          const PropellerCodeLayoutScorer &scorer) const;

  // Returns the total score contribution of edges running from `from_chain` to
  // `to_chain` for this assembly.
  double ComputeInterChainScore(const NodeToBundleMapper &bundle_mapper,
                                const PropellerCodeLayoutScorer &scorer,
                                const NodeChain &from_chain,
                                const NodeChain &to_chain) const;

  // Returns the total score gain from `split_chain()`'s intra-chain edges for
  // this assembly. This is more efficient than calling
  // `ComputeInterChainScore(scorer, chain, chain)` since it only computes the
  // delta in score from edges which run between different slices of
  // `split_chain()` (i.e., their source-to-sink distance has changed by
  // splitting).
  double ComputeSplitChainScoreGain(
      const NodeToBundleMapper &bundle_mapper,
      const PropellerCodeLayoutScorer &scorer) const;

  // Returns the score contribution of a single edge for this assembly.
  double ComputeEdgeScore(const NodeToBundleMapper &bundle_mapper,
                          const PropellerCodeLayoutScorer &scorer,
                          const CFGEdge &edge) const;

  // The two chains in the assembly.
  NodeChainPair chain_pair_;

  // The merge order of the slices
  ChainMergeOrder merge_order_;

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
  double score_gain_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDOLLVM_PROPELLER_NODE_CHAIN_ASSEMBLY_H_
