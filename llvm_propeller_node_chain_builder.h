#ifndef AUTOFDOLLVM_PROPELLER_NODE_CHAIN_BUILDER_H_
#define AUTOFDOLLVM_PROPELLER_NODE_CHAIN_BUILDER_H_

#include <iterator>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain.h"
#include "llvm_propeller_node_chain_assembly.h"
#include "llvm_propeller_statistics.h"
#include "base/logging.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"

namespace devtools_crosstool_autofdo {
// Comparator for comparing CFGNode pointers from a single CFG.
struct CFGNodePtrComparator {
  bool operator()(const CFGNode *a, const CFGNode *b) const {
    CHECK_NE(a, nullptr);
    CHECK_NE(b, nullptr);
    CHECK_EQ(a->function_index(), b->function_index());
    return a->intra_cfg_id() < b->intra_cfg_id();
  }
};

// Interface for storing NodeChainAssemblies in a priority queue.
class NodeChainAssemblyQueue {
 public:
  virtual ~NodeChainAssemblyQueue() = default;

  // Returns true iff there are any assemblies to be retrieved.
  virtual bool empty() const = 0;

  // Returns the best (highest-score) assembly. The behavior is undefined if
  // `empty()` returns `true`.
  virtual NodeChainAssembly GetBestAssembly() const = 0;

  // Removes the assembly associated with the ordered `NodeChain` pair
  // `<split_chain, unsplit_chain>`.
  virtual void RemoveAssembly(NodeChainPair chain_pair) = 0;

  // Inserts `assembly` into the queue. `assembly` is associated with the
  // `NodeChain` pair `<assembly.split_chain(), assembly.unsplit_chain>`. If an
  // assembly is already associated with this pair, it will be replaced by
  // `assembly`.
  virtual void InsertAssembly(NodeChainAssembly assembly) = 0;
};

// Balanced-binary-tree implementation for `NodeChainAssemblyQueue`.
// `GetBestAssembly` has constant time complexity
// `RemoveAssembly` and `InsertAssembly` have logarithmic time complexity.
class NodeChainAssemblyBalancedTreeQueue : public NodeChainAssemblyQueue {
 public:
  NodeChainAssembly GetBestAssembly() const override {
    return *assemblies_.rbegin();
  }

  bool empty() const override { return assemblies_.empty(); }

  void RemoveAssembly(NodeChainPair chain_pair) override {
    auto it = handles_.find(chain_pair);
    if (it == handles_.end()) return;
    assemblies_.erase(it->second);
    handles_.erase(it);
  }

  void InsertAssembly(NodeChainAssembly assembly) override {
    NodeChainPair chain_pair{.split_chain = &assembly.split_chain(),
                             .unsplit_chain = &assembly.unsplit_chain()};
    auto handle_it = handles_.find(chain_pair);
    if (handle_it != handles_.end()) assemblies_.erase(handle_it->second);
    handles_.insert_or_assign(chain_pair,
                              assemblies_.insert(std::move(assembly)).first);
  }

 private:
  // All `NodeChainAssembly` records, ordered by score.
  // Assemblies are stored in std::set which provides logarithmic-time
  // complexity insertions and deletion operations. It uses the score-based
  // comparator to allow retrieval of the max element in constant time.
  // Note that we can't use the recommended absl::btree_set because insertions
  // and deletions can invalidate all iterators.
  std::set<NodeChainAssembly, NodeChainAssembly::NodeChainAssemblyComparator>
      assemblies_;

  // Map from each `NodeChain` pair to its associated `NodeChainAssembly` record
  // in `assemblies_`. We use this map to lookup the `NodeChainAssembly`
  // record associated with each `NodeChain` pair. When one record in
  // `assemblies_` is deleted, its iterator in this map should also be removed
  // and vice versa. Other iterators will remain valid as this is guaranteed by
  // std::set.
  absl::flat_hash_map<
      NodeChainPair,
      std::set<NodeChainAssembly,
               NodeChainAssembly::NodeChainAssemblyComparator>::iterator>
      handles_;
};

// Iteration-based implementation of NodeChainAssemblyQueue.
// GetBestAssembly has linear-time complexity.
// `RemoveAssembly` and `InsertAssembly` have logarithmic-time complexity.
class NodeChainAssemblyIterativeQueue : public NodeChainAssemblyQueue {
 public:
  bool empty() const override { return assemblies_.empty(); }

  NodeChainAssembly GetBestAssembly() const override {
    auto best_assembly =
        absl::c_max_element(assemblies_, [](const auto &lhs, const auto &rhs) {
          return NodeChainAssembly::NodeChainAssemblyComparator()(lhs.second,
                                                                  rhs.second);
        });
    return best_assembly->second;
  }

  void RemoveAssembly(NodeChainPair chain_pair) override {
    assemblies_.erase(chain_pair);
  }

  void InsertAssembly(NodeChainAssembly assembly) override {
    assemblies_.insert_or_assign(assembly.chain_pair(), std::move(assembly));
  }

 private:
  absl::flat_hash_map<NodeChainPair, NodeChainAssembly> assemblies_;
};

// TODO(b/159842094): Make NodeChainBuilder exception-block aware.
// This class builds BB chains for one or multiple CFGs.
class NodeChainBuilder {
 public:
  // Creates and returns a `NodeChainBuilder` for the given `cfgs` with initial
  // chains specified by `initial_chains` (as a map from function indexes to
  // their initial chains given by vectors of bb indexes), code layout scorer
  // `scorer`, and code layout statistics handle `stats`.
  template <class AssemblyQueueImpl = NodeChainAssemblyIterativeQueue>
  static NodeChainBuilder CreateNodeChainBuilder(
      const PropellerCodeLayoutScorer &scorer,
      const std::vector<const ControlFlowGraph *> &cfgs,
      const absl::flat_hash_map<
          int, std::vector<std::vector<CFGNode::IntraCfgId>>> &initial_chains,
      PropellerStats::CodeLayoutStats &stats);

  const NodeToBundleMapper &node_to_bundle_mapper() const {
    return *node_to_bundle_mapper_;
  }

  const PropellerCodeLayoutScorer &code_layout_scorer() const {
    return code_layout_scorer_;
  }

  // Const public accessors for the internal data structures of chain. Used for
  // testing only.
  const std::vector<const ControlFlowGraph *> &cfgs() const { return cfgs_; }

  const absl::flat_hash_map<CFGNode::InterCfgId, std::unique_ptr<NodeChain>> &
  chains() const {
    return chains_;
  }

  const NodeChainAssemblyQueue &node_chain_assemblies() const {
    return *node_chain_assemblies_;
  }

  // This function initializes the chains and then iteratively constructs larger
  // chains by merging the best chains, to achieve the highest score.
  // Clients of this class must use this function after calling the constructor.
  std::vector<std::unique_ptr<NodeChain>> BuildChains();

  // function and they are public for testing only. Clients must use the
  // BuildChains function instead.

  // Initializes the basic block chains and bundles from nodes of the CFGs.
  void InitNodeChains();

  // Initializes the edges between chains from edges of the CFGs.
  void InitChainEdges();

  // Initializes the chain assemblies, which are all profitable ways of merging
  // chains together, with their scores.
  void InitChainAssemblies();

  // Coalesces all the built chains together to form a single chain.
  // NodeChainBuilder calls this function at the end to ensure that all hot
  // BB chains are placed together at the beginning of the function.
  void CoalesceChains();

  // Merges two chains according to a given chain assembly. The two chains are
  // merged into `assembly.split_chain()`. `assembly.unsplit_chain()` will be
  // garbage-collected at the end of this call. The provided assembly will be
  // killed by this call and shall not be used.
  void MergeChains(NodeChainAssembly assembly);

  // Merges `right_chain` to the right side of `left_chain`.
  void MergeChains(NodeChain &left_chain, NodeChain &right_chain);

 private:
  NodeChainBuilder(
      const PropellerCodeLayoutScorer &scorer,
      const std::vector<const ControlFlowGraph *> &cfgs,
      absl::flat_hash_map<int, std::vector<std::vector<CFGNode::IntraCfgId>>>
          initial_chains,
      PropellerStats::CodeLayoutStats &stats,
      std::unique_ptr<NodeChainAssemblyQueue> node_chain_assemblies)
      : code_layout_scorer_(scorer),
        cfgs_(cfgs),
        node_to_bundle_mapper_(
            NodeToBundleMapper::CreateNodeToBundleMapper(cfgs)),
        initial_chains_(std::move(initial_chains)),
        stats_(stats),
        node_chain_assemblies_(std::move(node_chain_assemblies)) {
    // Accept only one CFG for intra-function-ordering.
    if (!code_layout_scorer_.code_layout_params().inter_function_reordering())
      CHECK_EQ(cfgs_.size(), 1);
  }

  // Merges `inter_chain_out_edges_`, `inter_chain_in_edges`, and
  // `CFGNodeBundle::intra_chain_out_edges_` of the chain `merge_from_chain`
  // into `merge_to_chain`.
  void MergeChainEdges(NodeChain &merge_from_chain,
                       NodeChain &merge_to_chain) const;

  // Removes intra-bundle edges and updates `score_`.
  // After bundles are merged together, their `intra_chain_out_edges_` may
  // include intra-bundle edges. This function removes them while also deducting
  // their contribution to `score_` since intra-bundle edges should not
  // contribute to the chain's `score_`.
  void RemoveIntraBundleEdges(NodeChain &chain) const;

  // Updates and removes the assemblies for `kept_chain` and `defunct_chain`.
  // This method is called after `defunct_chain` is merged into `kept_chain` and
  // serves to update all assemblies between `kept_chain` and other chains and
  // also to remove all assemblies related to `defunct_chain`. `defunct_chain`
  // will be removed after this call.
  void UpdateAssembliesAfterMerge(NodeChain &kept_chain,
                                  NodeChain &defunct_chain);

  // Finds and updates the best (highest-gain) assembly for two chains.
  void UpdateNodeChainAssembly(NodeChain &split_chain,
                               NodeChain &unsplit_chain);

  // Returns whether `edge` should be considered in constructing the chains.
  bool ShouldVisitEdge(const CFGEdge &edge) {
    return edge.weight() != 0 && !edge.IsReturn() &&
           ((code_layout_scorer_.code_layout_params()
                 .inter_function_reordering() &&
             cfgs_.size() > 1 && !edge.inter_section()) ||
            !edge.IsCall());
  }

  const PropellerCodeLayoutScorer code_layout_scorer_;

  // CFGs targeted for BB chaining.
  const std::vector<const ControlFlowGraph *> cfgs_;

  std::unique_ptr<NodeToBundleMapper> node_to_bundle_mapper_;

  // Initial node chains, specified as a map from every function index to the
  // vector of initial node chains for the corresponding CFG. Each node chain is
  // specified by a vector of intra_cfg_ids of its nodes.
  const absl::flat_hash_map<int, std::vector<std::vector<CFGNode::IntraCfgId>>>
      initial_chains_;

  PropellerStats::CodeLayoutStats &stats_;

  // Constructed chains. This starts by having one chain for every CFGNode and
  // as chains keep merging together, defunct chains are removed from this.
  absl::flat_hash_map<CFGNode::InterCfgId, std::unique_ptr<NodeChain>> chains_;

  // Assembly (merge) candidates. This maps every pair of chains to its
  // (non-zero) merge score.
  std::unique_ptr<NodeChainAssemblyQueue> node_chain_assemblies_;
};

// Returns vectors of nodes which form forced-fallthrough paths. These are
// all the paths which -- based on the profile -- always execute from beginning
// to end once execution enters their source node.
// The idea here is that these edges must be attached together by the optimal
// layout, regardless of how other basic blocks are laid out. For example,
// consider the following CFG:
//            A      B
//           50\    /100
//              \  /
//               VV     150        150
//                C  --------> D --------> E
//                                        / \
//                                    20 /   \130
//                                      V     V
//                                      F     G
// The edges C->D and D->E can be bundled (frozen) in the layout independently
// of how other edges are treated by the layout algorithm.
// Note: The returned paths are sorted in increasing order of their first node's
// symbol ordinal.
std::vector<std::vector<const CFGNode *>> GetForcedPaths(
    const ControlFlowGraph &cfg);

// Returns all mutually-forced edges as a map from their source to their sink.
// These are the edges which -- based on the profile -- are the only outgoing
// edges from their source and the only incoming edges to their sinks.
// Note that the paths represented by these edges may include cycles.
absl::btree_map<const CFGNode *, const CFGNode *, CFGNodePtrComparator>
GetForcedEdges(const ControlFlowGraph &cfg);

// Breaks cycles in the `path_next` map by cutting the edge sinking to the
// smallest address in every cycle (hopefully a loop backedge).
void BreakCycles(absl::btree_map<const CFGNode *, const CFGNode *,
                                 CFGNodePtrComparator> &path_next);
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDOLLVM_PROPELLER_NODE_CHAIN_BUILDER_H_
