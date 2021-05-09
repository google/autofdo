#ifndef AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_BUILDER_H_
#define AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_BUILDER_H_

#include <map>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_node_chain.h"

namespace devtools_crosstool_autofdo {

// TODO(b/159842094): Make NodeChainBuilder exception-block aware.
// This class builds BB chains for one or multiple CFGs.
class NodeChainBuilder {
 public:
  NodeChainBuilder(const PropellerCodeLayoutScorer &scorer,
                   const std::vector<ControlFlowGraph *> &cfgs)
      : code_layout_scorer_(scorer),
        cfgs_(cfgs) {}

  NodeChainBuilder(const PropellerCodeLayoutScorer &scorer,
                   ControlFlowGraph *cfg)
      : code_layout_scorer_(scorer), cfgs_(1, cfg) {}

  // Const public accessors for the internal data structures of chain. Used for
  // testing only.
  const std::vector<ControlFlowGraph *> &cfgs() { return cfgs_; }

  const std::map<uint64_t, std::unique_ptr<NodeChain>> &chains() {
    return chains_;
  }

  const std::map<std::pair<NodeChain *, NodeChain *>, uint64_t>
      &node_chain_assemblies() {
    return node_chain_assemblies_;
  }

  // This function initializes the chains and then iteratively constructs larger
  // chains by merging the best chains, to achieve the highest score.
  // Clients of this class must use this function after calling the constructor.
  std::vector<std::unique_ptr<NodeChain>> BuildChains();

  // The following functions are the different stages of the BuildChains
  // function and they are public for testing only. Clients must use the
  // BuildChains function instead.

  // Initializes the basic block chains and bundles from nodes of the CFGs.
  void InitNodeChains();

  // Initializes the edges between chains from edges of the CFGs.
  void InitChainEdges();

  // Initializes the chain assemblies, which are all profitable ways of merging
  // chains together, with their scores.
  void InitChainAssemblies();

  // Iteratively applies the chain assembly having the maximum gain in the
  // score, and updates the assembly records accordingly, until the score cannot
  // be improved any further.
  void KeepMergingBestChains();

  // Coalesces all the built chains together to form a single chain.
  void CoalesceChains();

 private:
  const PropellerCodeLayoutScorer code_layout_scorer_;

  // CFGs targeted for BB chaining.
  const std::vector<ControlFlowGraph *> cfgs_;

  // Constructed chains. This starts by having one chain for every CFGNode and
  // as chains keep merging together, defunct chains are removed from this.
  std::map<uint64_t, std::unique_ptr<NodeChain>> chains_;

  // Assembly (merge) candidates. This maps every pair of chains to its
  // (non-zero) merge score.
  std::map<std::pair<NodeChain *, NodeChain *>, uint64_t>
      node_chain_assemblies_;

  void MergeChainEdges(NodeChain *source, NodeChain *destination);


  // Computes the score for a chain.
  uint64_t ComputeScore(NodeChain *chain) const;

  void MergeChains(NodeChain *left_chain, NodeChain *right_chain);

  void UpdateNodeChainAssembly(NodeChain *left_chain, NodeChain *right_chain);
};

}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_NODE_CHAIN_BUILDER_H_
