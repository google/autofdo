#ifndef AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_H_
#define AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_H_

#include <algorithm>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/btree_set.h"

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

class CFGNodeBundle {
  // TODO(b/160191690): Make classes' data members private and add accessors.
 public:
  // All the CFG nodes in this bundle.
  std::vector<CFGNode *> nodes_;

  // Containing chain for this bundle.
  NodeChain *chain_;

  // Index of the bundle in the chain.
  int chain_index_;

  // Offset at which this bundle is located in its containing chain.
  int64_t chain_offset_;

  // Total binary size of this bundle.
  uint64_t size_;

  // Total execution frequency of this bundle.
  int64_t freq_;

  // Edges from this bundle to other bundles of `chain_`. Ordered in increasing
  // order of the sink bundle's `chain_index_`. This ordering should be enforced
  // by `NodeChain::SortIntraChainEdges()` every time new edges are inserted.
  // This field allows an optimization for computing the score contribution of
  // intra-chain edges when the chain is split in a `NodeChainAssembly`.
  // Always initialized to empty since every chain is initialized as a single
  // bundle.
  std::vector<CFGEdge *> intra_chain_out_edges_ = {};

  // Constructor for building a bundle for a single CFG node and mapping it back
  // to its containing `chain`. `chain` must outlive the created CFGNodeBundle.
  CFGNodeBundle(CFGNode *n, NodeChain &chain, int chain_index,
                int64_t chain_offset)
      : nodes_(1, n),
        chain_(&chain),
        chain_index_(chain_index),
        chain_offset_(chain_offset),
        size_(n->size()),
        freq_(n->freq()) {
    n->set_bundle(this);
    n->set_bundle_offset(0);
  }

  // Constructor for building a bundle for a list of CFG nodes and mapping it
  // back to its containing `chain`. `chain` must outlive the created
  // `CFGNodeBundle`.
  explicit CFGNodeBundle(std::vector<CFGNode *> nodes, NodeChain &chain,
                int chain_index, int64_t chain_offset)
      : nodes_(std::move(nodes)),
        chain_(&chain),
        chain_index_(chain_index),
        chain_offset_(chain_offset),
        size_(0),
        freq_(0) {
    for (CFGNode *n : nodes_) {
      n->set_bundle(this);
      n->set_bundle_offset(size_);
      size_ += n->size_;
      freq_ += n->freq_;
    }
  }

  // Iterates over all nodes in this bundle (in order) while calling `func` on
  // each node.
  void VisitEachNodeRef(absl::FunctionRef<void(const CFGNode &)> func) const {
    for (CFGNode *node : nodes_)
      func(*node);
  }

  // Merges the `other` CFGNodeBundle into `*this` by moving `other.nodes_`
  // to the end of `this->nodes_`.
  void MergeWith(CFGNodeBundle other) {
    for (CFGNode *node : other.nodes_) {
      node->bundle_ = this;
      node->bundle_offset_ += size_;
    }
    absl::c_move(std::move(other.nodes_), std::back_inserter(nodes_));
    size_ += other.size_;
    freq_ += other.freq_;
    absl::c_move(std::move(other.intra_chain_out_edges_),
                 std::back_inserter(intra_chain_out_edges_));
  }
};

// Represents a chain of nodes (basic blocks).
class NodeChain {
  // TODO(b/160191690): Make classes' data members private and add accessors.
 public:
  // Representative node for the chain.
  CFGNode *delegate_node_;

  // ControlFlowGraph of the nodes in this chain (This will be null if the nodes
  // come from more than one cfg).
  ControlFlowGraph *cfg_;

  // Ordered list of the bundles of the chain.
  std::vector<std::unique_ptr<CFGNodeBundle>> node_bundles_;
  // Total binary size of the chain.
  uint64_t size_;
  // Total execution frequency of the chain.
  int64_t freq_;
  // Total score for this chain.
  int64_t score_ = 0;

  struct RefComparator {
    bool operator()(const NodeChain &nc1, const NodeChain &nc2) const {
      return nc1.id() < nc2.id();
    }
  };

  struct PtrComparator {
    bool operator()(const NodeChain *nc1, const NodeChain *nc2) const {
      return RefComparator()(*nc1, *nc2);
    }
  };

  // Each map key is a NodeChain which has (at least) one CFGNode that is
  // the sink node of an edge in `inter_outs_` or `intra_outs_` of CFGNodes in
  // "this" NodeChain. The corresponding map value is the collection of CFGEdges
  // which have have a sink node equal to one of the CFGNodes of the
  // corresponding map key. Note, we use "NodeChain::PtrComparator" to make sure
  // the iterating order is deterministic. Note, intra-chain edges won't be
  // inserted in this map, i.e., this map cannot have "this" NodeChain as a key.
  std::map<NodeChain *, std::vector<CFGEdge *>, NodeChain::PtrComparator>
      inter_chain_out_edges_;

  // Chains which have outgoing edges to `*this`. We use
  // `NodeChain::PtrComparator` to make sure the iterating order is
  // deterministic.
  absl::btree_set<NodeChain *, NodeChain::PtrComparator> inter_chain_in_edges_;

  uint64_t id() const { return delegate_node_->symbol_ordinal(); }

  // Gives the execution density for this chain.
  double exec_density() const {
    return (static_cast<double>(freq_)) /
           std::max(size_, static_cast<uint64_t>(1));
  }

  CFGNode *GetLastNode() const {
    return node_bundles_.back()->nodes_.back();
  }

  CFGNode *GetFirstNode() const {
    return node_bundles_.front()->nodes_.front();
  }

  // Constructor for building a chain from a vector of nodes, all placed in one
  // bundle.
  explicit NodeChain(std::vector<CFGNode *> nodes) {
    delegate_node_ = nodes.front();
    node_bundles_.push_back(
        std::make_unique<CFGNodeBundle>(std::move(nodes), *this, 0, 0));
    cfg_ = delegate_node_->cfg();
    size_ = node_bundles_.front()->size_;
    freq_ = node_bundles_.front()->freq_;
  }

  // Merges `inter_chain_out_edges_`, `inter_chain_in_edges`, and
  // `CFGNodeBundle::intra_chain_out_edges_` of the chain `other` into `*this`.
  void MergeChainEdges(NodeChain &other);

  // This moves the bundles from another chain into this chain and updates the
  // bundle and chain fields accordingly. After this is called, the 'other'
  // chain becomes empty.
  void MergeWith(NodeChain &other) {
    for (auto &bundle : other.node_bundles_) {
      bundle->chain_ = this;
      bundle->chain_offset_ += size_;
      bundle->chain_index_ += node_bundles_.size();
    }
    std::move(other.node_bundles_.begin(), other.node_bundles_.end(),
              std::back_inserter(node_bundles_));
    size_ += other.size_;
    freq_ += other.freq_;
    // Nullify cfg_ if the other chain's nodes come from a different CFG.
    if (cfg_ && cfg_ != other.cfg_) cfg_ = nullptr;
  }

  // Given a NodeChainAssembly `assembly`, merges `assembly.unsplit_chain()`
  // into `*this` chain and reassembles it based on the order specified by
  // `assembly`.
  // Note, requires `this == &assembly.split_chain()`.
  void MergeWith(NodeChainAssembly assembly,
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
  void SortIntraChainEdges() {
    for (std::unique_ptr<CFGNodeBundle> &bundle : node_bundles_) {
      // Sort edges based on the position of the sink node's bundle in the
      // chain.
      absl::c_sort(bundle->intra_chain_out_edges_, [](const CFGEdge *e1,
                                                      const CFGEdge *e2) {
        return std::forward_as_tuple(e1->sink()->bundle()->chain_index_,
                                     e1->src()->symbol_ordinal()) <
               std::forward_as_tuple(e2->sink()->bundle()->chain_index_,
                                     e2->src()->symbol_ordinal());
      });
    }
  }

  // Removes intra-bundle edges and updates `score_`.
  // After bundles are merged together, their `intra_chain_out_edges_` may
  // include intra-bundle edges. This function removes them while also deducting
  // their contribution to `score_` since intra-bundle edges should not
  // contribute to the chain's `score_`.
  void RemoveIntraBundleEdges(const PropellerCodeLayoutScorer &scorer);
};

// Gets the chain containing a given node.
NodeChain &GetNodeChain(const CFGNode *n);

// Gets the offset of a node in its containing chain.
int64_t GetNodeOffset(const CFGNode *n);
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_H_
