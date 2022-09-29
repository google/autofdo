#ifndef AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_BUILDER_H_
#define AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_BUILDER_H_

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain.h"
#include "llvm_propeller_node_chain_assembly.h"

namespace devtools_crosstool_autofdo {

// TODO(b/159842094): Make NodeChainBuilder exception-block aware.
// This class builds BB chains for one or multiple CFGs.
class NodeChainBuilder {
 public:
  NodeChainBuilder(const PropellerCodeLayoutScorer &scorer,
                   const std::vector<ControlFlowGraph *> &cfgs,
                   CodeLayoutStats &stats)
      : code_layout_scorer_(scorer), cfgs_(cfgs), stats_(stats) {}

  NodeChainBuilder(const PropellerCodeLayoutScorer &scorer,
                   ControlFlowGraph &cfg, CodeLayoutStats &stats)
      : code_layout_scorer_(scorer), cfgs_(1, &cfg), stats_(stats) {}

  const PropellerCodeLayoutScorer &code_layout_scorer() const {
    return code_layout_scorer_;
  }

  // Const public accessors for the internal data structures of chain. Used for
  // testing only.
  const std::vector<ControlFlowGraph *> &cfgs() const { return cfgs_; }

  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains() const {
    return chains_;
  }

  const std::map<std::pair<NodeChain *, NodeChain *>, NodeChainAssembly>
      &node_chain_assemblies() const {
    return node_chain_assemblies_;
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

  // Returns the highest-gain assembly among all chains.
  NodeChainAssembly GetBestAssembly() const;

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
  // Merges the in-and-out chain-edges of the chain `source`
  // into those of the chain `destination`.
  void MergeChainEdges(NodeChain &source, NodeChain &destination);

  // Computes the score for a chain.
  int64_t ComputeScore(NodeChain &chain) const;

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

  const PropellerCodeLayoutScorer code_layout_scorer_;

  // CFGs targeted for BB chaining.
  const std::vector<ControlFlowGraph *> cfgs_;

  CodeLayoutStats &stats_;

  // Constructed chains. This starts by having one chain for every CFGNode and
  // as chains keep merging together, defunct chains are removed from this.
  std::map<uint64_t, std::unique_ptr<NodeChain>> chains_;

  // Assembly (merge) candidates. This maps every pair of chains to its
  // (non-zero) merge score.
  std::map<std::pair<NodeChain *, NodeChain *>, NodeChainAssembly>
      node_chain_assemblies_;
};

}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_BUILDER_H_
