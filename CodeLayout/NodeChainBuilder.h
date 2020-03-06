//===- PropellerNodeChainBuilder.h  ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLD_ELF_PROPELLER_CODE_LAYOUT_NODE_CHAIN_BUILDER_H
#define LLD_ELF_PROPELLER_CODE_LAYOUT_NODE_CHAIN_BUILDER_H

#include "ModifiablePriorityQueue.h"
#include "NodeChain.h"
#include "NodeChainAssembly.h"
#include "NodeChainClustering.h"
#include "PropellerCFG.h"

#include "llvm/ADT/DenseMap.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

using llvm::DenseMap;

namespace lld {
namespace propeller {

// bb chain builder based on the ExtTSP metric
class NodeChainBuilder {
private:
  NodeChainAssembly::CompareNodeChainAssembly nodeChainAssemblyComparator;
  // Cfgs repreresenting the functions that are reordered
  std::vector<ControlFlowGraph *> cfgs;

  // Set of built chains, keyed by section index of their Delegate nodes.
  // chains are removed from this Map once they are merged into other chains.
  DenseMap<uint64_t, std::unique_ptr<NodeChain>> chains;

  // All the initial chains, seperated into connected components
  std::vector<std::vector<NodeChain *>> components;

  // NodeChainBuilder performs bb ordering component by component.
  // This is the component number that the chain builder is currently working
  // on.
  unsigned currentComponent;

  // These represent all the edges which are -- based on the profile -- the only
  // (executed) outgoing edges from their source node and the only (executed)
  // incoming edges to their sink nodes. The algorithm will make sure that these
  // edges form fall-throughs in the final order.
  // DenseMap<CFGNode *, CFGNode *> mutuallyForcedOut;

  // This maps every (ordered) pair of chains (with the first chain in the pair
  // potentially splittable) to the highest-gain NodeChainAssembly for those
  // chains. The Heap data structure allows fast retrieval of the maximum gain
  // NodeChainAssembly, along with fast update.
  ModifiablePriorityQueue<std::pair<NodeChain *, NodeChain *>,
                          std::unique_ptr<NodeChainAssembly>,
                          std::less<std::pair<NodeChain *, NodeChain *>>,
                          NodeChainAssembly::CompareNodeChainAssembly>
      nodeChainAssemblies;

  // This map stores the candidate chains for each chain.
  //
  // For every chain, its candidate chains are the chains which can increase the
  // overall ExtTSP score when merged with that chain. This is used to update
  // the nodeChainAssemblies map whenever chains merge together. The candidate
  // chains of a chain may also be updated as result of a merge.
  std::unordered_map<NodeChain *, std::unordered_set<NodeChain *>>
      candidateChains;

  void coalesceChains();
  void initializeExtTSP();

  // This function separates builds the connected components for the chains so
  // it can later merge each connected component separately.
  // chains which are not connected to each other via profile edges will never
  // be merged together by NodeChainBuilder. However, the composed chains may
  // later be interleaved by ChainClustering.
  void initializeChainComponents();
  void attachFallThroughs();

  // This function tries to place two nodes immediately adjacent to
  // each other (used for fallthroughs).
  // Returns true if this can be done.
  bool attachNodes(CFGNode *src, CFGNode *sink);

  void mergeChainEdges(NodeChain *splitChain, NodeChain *unsplitChain);

  void mergeInOutEdges(NodeChain *mergerChain, NodeChain *mergeeChain);
  void mergeChains(NodeChain *leftChain, NodeChain *rightChain);
  void mergeChains(std::unique_ptr<NodeChainAssembly> assembly);

  // Recompute the ExtTSP score of a chain
  uint64_t computeExtTSPScore(NodeChain *chain) const;

  void adjustExtTSPScore(NodeChain *chain) const;

  // Update the related NodeChainAssembly records for two chains, with the
  // assumption that unsplitChain has been merged into splitChain.
  bool updateNodeChainAssembly(NodeChain *splitChain, NodeChain *unsplitChain);

  void mergeAllChains();

  void init();

  // Initialize the bundles
  void initBundles(ControlFlowGraph &cfg,
                   std::vector<std::vector<CFGNode *>> &bundles);

  // Initialize basic block chains and bundles with one chain for every bundle,
  // every vector in paths would constitute a bundle which is gauaranteed to
  // remain fixed (not splitted) during the chain building.
  void initNodeChains(ControlFlowGraph &cfg,
                      std::vector<std::vector<CFGNode *>> &bundles);

public:
  // This invokes the Extended TSP algorithm, orders the hot and cold basic
  // blocks and inserts their associated symbols at the corresponding locations
  // specified by the parameters (HotPlaceHolder and ColdPlaceHolder) in the
  // given SymbolList.
  void doOrder(std::unique_ptr<ChainClustering> &clustering);

  NodeChainBuilder(std::vector<ControlFlowGraph *> &cfgs) : cfgs(cfgs) {}

  NodeChainBuilder(ControlFlowGraph *cfg) : cfgs(1, cfg) {}
};

} // namespace propeller
} // namespace lld

#endif
