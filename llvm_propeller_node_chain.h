#ifndef AUTOFDOLLVM_PROPELLER_NODE_CHAIN_H_
#define AUTOFDOLLVM_PROPELLER_NODE_CHAIN_H_

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"

// A node chain represents an ordered list of CFG nodes, which are further
// split into multiple (ordered) list of nodes called bundles. For example
//
//   * bundle1 {foo -> foo.1} -> bundle2 {bar} -> bundle3 {foo.2 -> foo.3}
//
// represents a chain for 4 nodes from function "foo" and one node from "bar".
// The nodes are grouped into three bundles: two bundles for "foo" and one
// bundle for "bar".
// NodeChainBuilder keeps merging chains together to form longer chains.
// Merging may be done by splitting one chains and shoving the other chain
// in between, but it cannot break any bundles. For instance, the chain above
// can only be split across the two bundle-joining parts (bundle1 and bundle2
// or bundle2 and bundle3). NodeChainBuilder can also "bundle-up" multiple
// bundles into a single bundle when no gains are foreseen from splitting those
// bundles.
//
// At each point in time, every node belongs to exactly one bundle which is
// contained in exactly one chain.

namespace devtools_crosstool_autofdo {
class NodeChain;
class NodeChainAssembly;
class NodeToBundleMapper;

class CFGNodeBundle {
 public:
  // This struct contains information about the position of a `CFGNodeBundle`
  // inside its containing `NodeChain`.
  struct ChainMappingEntry {
    // Containing chain for this bundle.
    NodeChain *chain;
    // Index of the bundle in `chain->node_bundles()`.
    int chain_index;
    // Binary size offset at which this bundle is located in its containing
    // chain.
    int chain_offset;
  };

  // Constructor for building a bundle for a single CFG node and mapping it back
  // to its containing `chain`. `chain` must outlive the created CFGNodeBundle.
  CFGNodeBundle(const CFGNode *n, NodeChain &chain, int chain_index,
                int chain_offset)

      : nodes_(1, n),
        chain_mapping_({.chain = &chain,
                        .chain_index = chain_index,
                        .chain_offset = chain_offset}),
        size_(n->size()),
        freq_(n->CalculateFrequency()) {}

  // Constructor for building a bundle for a list of CFG nodes and mapping it
  // back to its containing `chain`. `chain` must outlive the created
  // `CFGNodeBundle`.
  explicit CFGNodeBundle(std::vector<const CFGNode *> nodes, NodeChain &chain,
                         int chain_index, int chain_offset)
      : nodes_(std::move(nodes)),
        chain_mapping_({.chain = &chain,
                        .chain_index = chain_index,
                        .chain_offset = chain_offset}),

        size_(0),
        freq_(0) {
    for (const CFGNode *n : nodes_) {
      size_ += n->size();
      freq_ += n->CalculateFrequency();
    }
  }

  // CFGNodeBundle is a moveonly object.
  CFGNodeBundle(const CFGNodeBundle &other) = delete;
  CFGNodeBundle &operator=(const CFGNodeBundle &other) = delete;
  CFGNodeBundle(CFGNodeBundle &&other) = default;
  CFGNodeBundle &operator=(CFGNodeBundle &&other) = default;

  int size() const { return size_; }
  int freq() const { return freq_; }
  const std::vector<const CFGNode *> &nodes() const { return nodes_; }
  const ChainMappingEntry &chain_mapping() const { return chain_mapping_; }
  const std::vector<const CFGEdge *> &intra_chain_out_edges() const {
    return intra_chain_out_edges_;
  }
  std::vector<const CFGEdge *> &mutable_intra_chain_out_edges() {
    return intra_chain_out_edges_;
  }

  // Adjusts `chain_mapping` by setting the containing chain and
  // applying the displacement specified by
  // `chain_mapping_adjustment` to `chain_mapping_`.
  void AdjustChainMappingEntry(ChainMappingEntry chain_mapping_adjustment) {
    chain_mapping_.chain = chain_mapping_adjustment.chain;
    chain_mapping_.chain_index += chain_mapping_adjustment.chain_index;
    chain_mapping_.chain_offset += chain_mapping_adjustment.chain_offset;
  }

  void SetChainMappingEntry(ChainMappingEntry chain_mapping) {
    chain_mapping_ = chain_mapping;
  }

  void SortIntraChainEdges(const NodeToBundleMapper &node_to_bundle_mapper);

  // Iterates over all nodes in this bundle (in order) while calling `func` on
  // each node.
  void VisitEachNodeRef(absl::FunctionRef<void(const CFGNode &)> func) const {
    for (const CFGNode *node : nodes_) func(*node);
  }

  // Merges the `other` CFGNodeBundle into `*this` by moving `other.nodes_`
  // to the end of `this->nodes_`.
  void MergeWith(CFGNodeBundle &other) {
    absl::c_move(std::move(other.nodes_), std::back_inserter(nodes_));
    size_ += other.size_;
    freq_ += other.freq_;
    absl::c_move(std::move(other.intra_chain_out_edges_),
                 std::back_inserter(intra_chain_out_edges_));
  }

 private:
  // All the CFG nodes in this bundle.
  std::vector<const CFGNode *> nodes_;

  ChainMappingEntry chain_mapping_;

  // Total binary size of this bundle.
  int size_;

  // Total execution frequency of this bundle.
  int freq_;

  // Edges from this bundle to other bundles of `chain_`. Ordered in increasing
  // order of the sink bundle's `chain_index_`. This ordering should be enforced
  // by `NodeChain::SortIntraChainEdges()` every time new edges are inserted.
  // This field allows an optimization for computing the score contribution of
  // intra-chain edges when the chain is split in a `NodeChainAssembly`.
  // Always initialized to empty since every chain is initialized as a single
  // bundle.
  std::vector<const CFGEdge *> intra_chain_out_edges_ = {};
};

// Represents a chain of nodes (basic blocks).
class NodeChain {
 public:
  // Enum class to describe how to bundle the nodes in a chain.
  enum class BundleMode { kBundleAllNodesTogether, kBundleEachSingleNode };

  // Constructor for building a chain from a vector of nodes and with bundling
  // mode specified by `bundle_mode`. If `bundle_mode ==
  // BundleMode::kBundleAllNodesTogether` nodes will be placed in one
  // `CFGNodeBundle`. Otherwise (when `bundle_mode ==
  // BundleMode::kBundleEachSingleNode`) each node is placed in its own
  // `CFGNodeBundle`.
  NodeChain(std::vector<const CFGNode *> nodes, BundleMode bundle_mode)
      : id_(nodes.front()->inter_cfg_id()),
        function_index_(nodes.front()->function_index()) {
    switch (bundle_mode) {
      case BundleMode::kBundleAllNodesTogether:
        node_bundles_.push_back(
            std::make_unique<CFGNodeBundle>(std::move(nodes), *this, 0, 0));
        size_ = node_bundles_.front()->size();
        freq_ = node_bundles_.front()->freq();
        return;
      case BundleMode::kBundleEachSingleNode:
        int chain_index = 0;
        int chain_offset = 0;
        freq_ = 0;
        for (const CFGNode *node : nodes) {
          node_bundles_.push_back(std::make_unique<CFGNodeBundle>(
              node, *this, chain_index++, chain_offset));
          chain_offset += node->size();
          freq_ += node->CalculateFrequency();
        }
        size_ = chain_offset;
        return;
    }
  }

  // NodeChain is a moveable-only object.
  NodeChain(const NodeChain &other) = delete;
  NodeChain &operator=(const NodeChain &other) = delete;
  NodeChain(NodeChain &&other) = default;
  NodeChain &operator=(NodeChain &&other) = default;

  const CFGNode::InterCfgId &id() const { return id_; }
  std::optional<int> function_index() const { return function_index_; }
  double score() const { return score_; }
  int size() const { return size_; }
  int freq() const { return freq_; }
  std::vector<std::unique_ptr<CFGNodeBundle>> &mutable_node_bundles() {
    return node_bundles_;
  }
  const std::vector<std::unique_ptr<CFGNodeBundle>> &node_bundles() const {
    return node_bundles_;
  }
  absl::flat_hash_map<NodeChain *, std::vector<const CFGEdge *>> &
  mutable_inter_chain_out_edges() {
    return inter_chain_out_edges_;
  }
  absl::flat_hash_set<NodeChain *> &mutable_inter_chain_in_edges() {
    return inter_chain_in_edges_;
  }
  const absl::flat_hash_map<NodeChain *, std::vector<const CFGEdge *>> &
  inter_chain_out_edges() const {
    return inter_chain_out_edges_;
  }
  const absl::flat_hash_set<NodeChain *> &inter_chain_in_edges() const {
    return inter_chain_in_edges_;
  }

  // Gives the execution density for this chain.
  double exec_density() const {
    return (static_cast<double>(freq_)) / std::max(size_, 1);
  }

  // Calculates the total score of this chain based on `scorer`. This function
  // aggregates the score of all edges whose src and sink belong to this chain.
  double ComputeScore(const NodeToBundleMapper &bundle_mapper,
                      const PropellerCodeLayoutScorer &scorer) const;

  void SetScore(double score) { score_ = score; }

  const CFGNode *GetLastNode() const {
    return node_bundles_.back()->nodes().back();
  }

  const CFGNode *GetFirstNode() const {
    return node_bundles_.front()->nodes().front();
  }

  // Merges `inter_chain_out_edges_`, `inter_chain_in_edges`, and
  // `CFGNodeBundle::intra_chain_out_edges_` of the chain `other` into `*this`.
  void MergeChainEdges(NodeChain &other,
                       const NodeToBundleMapper &node_to_bundle_mapper);

  // This moves the bundles from another chain into this chain and updates the
  // bundle and chain fields accordingly. After this is called, the 'other'
  // chain becomes empty.
  void MergeWith(NodeChain &other) {
    for (auto &bundle : other.node_bundles_) {
      bundle->AdjustChainMappingEntry(
          {.chain = this,
           .chain_index = static_cast<int>(node_bundles_.size()),
           .chain_offset = size_});
    }
    std::move(other.node_bundles_.begin(), other.node_bundles_.end(),
              std::back_inserter(node_bundles_));
    size_ += other.size_;
    freq_ += other.freq_;
    // Nullify function_index_ if the other chain's nodes come from a different
    // CFG.
    if (function_index_ != other.function_index_) function_index_.reset();
  }

  // Given a NodeChainAssembly `assembly`, merges `assembly.unsplit_chain()`
  // into `*this` chain and reassembles it based on the order specified by
  // `assembly`.
  // Note, requires `this == &assembly.split_chain()`.
  void MergeWith(NodeChainAssembly assembly,
                 NodeToBundleMapper &node_to_bundle_mapper,
                 const PropellerCodeLayoutScorer &scorer);
  // Visit each candidate chain of this chain. This includes all the chains
  // that this chain has edges to or from, excluding itself.
  void VisitEachCandidateChain(absl::FunctionRef<void(NodeChain *)> func) {
    // Visit chains having edges *to* this chain.
    for (NodeChain *chain : inter_chain_in_edges_) func(chain);
    // Visit chains having edges *from* this chain, excluding those visited
    // above.
    for (const auto &[to_chain, unused] : inter_chain_out_edges_) {
      // Chains having edges *to* this chain are already visited above.
      if (inter_chain_in_edges_.contains(to_chain)) continue;
      func(to_chain);
    }
  }

  // Helper method for iterating over all nodes in this chain (in order).
  void VisitEachNodeRef(absl::FunctionRef<void(const CFGNode &)> func) const {
    for (const std::unique_ptr<CFGNodeBundle> &bundle_ptr : node_bundles_)
      bundle_ptr->VisitEachNodeRef(func);
  }

  // Sorts `intra_chain_out_edges_` of every bundle in the chain. This should be
  // called every time `intra_chain_out_edges_` is modified.
  void SortIntraChainEdges(const NodeToBundleMapper &node_to_bundle_mapper);

  // Removes intra-bundle edges and updates `score_`.
  // After bundles are merged together, their `intra_chain_out_edges_` may
  // include intra-bundle edges. This function removes them while also deducting
  // their contribution to `score_` since intra-bundle edges should not
  // contribute to the chain's `score_`.
  void RemoveIntraBundleEdges(
      const NodeToBundleMapper &bundle_mapper,
      const PropellerCodeLayoutScorer &code_layout_scorer);

 private:
  CFGNode::InterCfgId id_;
  // function_index of CFG corresponding to the nodes in this chain (or
  // std::nullopt if nodes are from different CFGs).
  std::optional<int> function_index_;

  // Ordered list of the bundles of the chain.
  std::vector<std::unique_ptr<CFGNodeBundle>> node_bundles_;
  // Total binary size of the chain.
  int size_;
  // Total execution frequency of the chain.
  int freq_;
  // Total score for this chain.
  double score_ = 0;

  // Each map key is a NodeChain which has (at least) one CFGNode that is
  // the sink node of an edge in `inter_outs_` or `intra_outs_` of CFGNodes in
  // "this" NodeChain. The corresponding map value is the collection of CFGEdges
  // which have have a sink node equal to one of the CFGNodes of the
  // corresponding map key. Note that intra-chain edges won't be
  // inserted in this map, i.e., this map cannot have "this" NodeChain as a key.
  absl::flat_hash_map<NodeChain *, std::vector<const CFGEdge *>>
      inter_chain_out_edges_;

  // Chains which have outgoing edges to `*this`.
  absl::flat_hash_set<NodeChain *> inter_chain_in_edges_;
};

// Maps `CFGNode`s to their associated bundles during the chain building
// process.
class NodeToBundleMapper {
 public:
  // This struct contains information about the position of a `CFGNode`
  // within its containing `CFGNodebundle`.
  struct BundleMappingEntry {
    // Containing `CFGNodeBundle`.
    CFGNodeBundle *bundle = nullptr;
    // Binary offset within `bundle`.
    int bundle_offset = 0;

    // Returns the offset of the node in its containing chain.
    int GetNodeOffset() const {
      return bundle->chain_mapping().chain_offset + bundle_offset;
    }
  };

  // Creates and returns a `NodeToBundleMapper` for mapping nodes of CFGs in
  // `cfgs`.
  static std::unique_ptr<NodeToBundleMapper> CreateNodeToBundleMapper(
      const std::vector<const ControlFlowGraph *> &cfgs);

  virtual ~NodeToBundleMapper() = default;

  // NodeToBundleMapper is non-copyable and non-movable.
  NodeToBundleMapper(const NodeToBundleMapper &) = delete;
  NodeToBundleMapper &operator=(const NodeToBundleMapper &) = delete;
  NodeToBundleMapper(NodeToBundleMapper &&) = delete;
  NodeToBundleMapper &operator=(NodeToBundleMapper &&) = delete;

  // Implementation of this function must return the index associated with
  // `node` in `node_bundle_mapping_`.
  virtual int GetNodeIndex(const CFGNode *node) const = 0;

  const BundleMappingEntry &GetBundleMappingEntry(int node_index) const {
    return node_bundle_mapping_[node_index];
  }

  const BundleMappingEntry &GetBundleMappingEntry(const CFGNode *node) const {
    return GetBundleMappingEntry(GetNodeIndex(node));
  }

  // Returns the offset of node associated with `node_index` in its chain.
  int GetNodeOffset(int node_index) const {
    return GetBundleMappingEntry(node_index).GetNodeOffset();
  }

  // Returns the offset of CFGNode `node` in its containing chain.
  int GetNodeOffset(CFGNode *node) const {
    return GetNodeOffset(GetNodeIndex(node));
  }

  // Sets the BundleMappingEntry associated with `node` equal to
  // `bundle_mapping`.
  void SetBundleMappingEntry(const CFGNode *node,
                             BundleMappingEntry bundle_mapping) {
    int node_index = GetNodeIndex(node);
    node_bundle_mapping_[node_index] = std::move(bundle_mapping);
  }

  // Adjusts the BundleMappingEntry associated with `node` according to
  // `bundle_mapping`, where its bundle_offset is incremented by
  // `bundle_mapping.bundle_offset`.
  void AdjustBundleMappingEntry(const CFGNode *node,
                                const BundleMappingEntry &bundle_mapping) {
    int node_index = GetNodeIndex(node);
    auto &current_bundle_mapping = node_bundle_mapping_[node_index];
    current_bundle_mapping.bundle = bundle_mapping.bundle;
    current_bundle_mapping.bundle_offset += bundle_mapping.bundle_offset;
  }

 protected:
  explicit NodeToBundleMapper(int index_range)
      : node_bundle_mapping_(index_range) {}

 private:
  // Stores the `BundleMappingEntry` object associated with each node.
  // For each CFGNode `node`, the information must be in
  // `node_bundle_mapping_[GetNodeIndex(node)]`.
  std::vector<BundleMappingEntry> node_bundle_mapping_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDOLLVM_PROPELLER_NODE_CHAIN_H_
