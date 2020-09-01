//===- PropellerNodeChainAssembly.h  --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This file includes the declaration of the NodeChainAssembly class. Each
// instance of this class gives a recipe for merging two NodeChains together in
// addition to the ExtTSP score gain that will be achieved by that merge. Each
// NodeChainAssembly consists of three NodeChainSlices from two node chains: the
// (potentially) splitted chain, and the unsplit chain. A NodeChainSlice has
// represents a slice of a NodeChain by storing iterators to the beginning and
// end of that slice in the node chain, plus the binary offsets at which these
// slices begin and end. The offsets allow effient computation of the gain in
// ExtTSP score.
//===----------------------------------------------------------------------===//
#ifndef LLD_ELF_PROPELLER_CODE_LAYOUT_NODE_CHAIN_ASSEMBLY_H
#define LLD_ELF_PROPELLER_CODE_LAYOUT_NODE_CHAIN_ASSEMBLY_H

#include "NodeChain.h"
#include "PropellerCFG.h"

#include <list>
#include <vector>

namespace llvm {
namespace propeller {

uint64_t getEdgeExtTSPScore(const CFGEdge &edge, int64_t srcSinkDistance);

// This class defines a slices of a node chain, specified by iterators to the
// beginning and end of the slice.
class NodeChainSlice {
public:
  // chain from which this slice comes from
  NodeChain *chain;

  // The endpoints of the slice in the corresponding chain
  std::list<std::unique_ptr<CFGNodeBundle>>::iterator beginPosition,
      endPosition;

  // The offsets corresponding to the two endpoints
  int64_t beginOffset, endOffset;

  // Constructor for building a chain slice from a given chain and the two
  // endpoints of the chain.
  NodeChainSlice(NodeChain *c,
                 std::list<std::unique_ptr<CFGNodeBundle>>::iterator begin,
                 std::list<std::unique_ptr<CFGNodeBundle>>::iterator end)
      : chain(c), beginPosition(begin), endPosition(end) {

    beginOffset = (*begin)->chainOffset;
    if (endPosition == chain->nodeBundles.end())
      endOffset = chain->size;
    else
      endOffset = (*end)->chainOffset;
  }

  // Constructor for building a chain slice from a node chain containing all of
  // its nodes.
  NodeChainSlice(NodeChain *c)
      : chain(c), beginPosition(c->nodeBundles.begin()),
        endPosition(c->nodeBundles.end()), beginOffset(0), endOffset(c->size) {}

  // (Binary) size of this slice
  int64_t size() const { return endOffset - beginOffset; }

  bool isEmpty() const { return beginPosition == endPosition; }
};

// This enum represents the order in which three slices (S1, S2, and U) are
// merged together. We exclude S1S2U and US1S2 since they are respectively
// equivalent to S2S1U (with S2 being empty) and U2U1S (with U2 being empty).
enum MergeOrder {
  Begin,
  S2S1U = Begin,
  BeginNext,
  S1US2 = BeginNext,
  S2US1,
  US2S1,
  End
};

// This class defines a strategy for assembling two chains together with one of
// the chains potentially split into two chains.
class NodeChainAssembly {
public:
  // The gain in ExtTSP score achieved by this NodeChainAssembly once it
  // is accordingly applied to the two chains.
  // This is effectively equal to "score - splitChain->score -
  // unsplitChain->score".
  uint64_t scoreGain = 0;

  // The two chains, the first being the splitChain and the second being the
  // unsplitChain.
  std::pair<NodeChain *, NodeChain *> chainPair;

  // The splitting position in splitchain
  std::list<std::unique_ptr<CFGNodeBundle>>::iterator slicePosition;

  // The three chain slices
  std::vector<NodeChainSlice> slices;

  // The merge order of the slices
  MergeOrder mergeOrder;

  bool splits, splitsAtFunctionTransition, needsSplitChainRotation, needsBundling;

  // The constructor for creating a NodeChainAssembly. slicePosition must be an
  // iterator into splitChain->nodes.
  NodeChainAssembly(
      NodeChain *splitChain, NodeChain *unsplitChain,
      std::list<std::unique_ptr<CFGNodeBundle>>::iterator slicePosition,
      MergeOrder mOrder)
      : chainPair(splitChain, unsplitChain), slicePosition(slicePosition),
        mergeOrder(mOrder) {
    // Construct the slices.
    NodeChainSlice s1(splitChain, splitChain->nodeBundles.begin(),
                      slicePosition);
    NodeChainSlice s2(splitChain, slicePosition, splitChain->nodeBundles.end());
    NodeChainSlice u(unsplitChain);

    switch (mergeOrder) {
    case MergeOrder::S2S1U:
      slices = {std::move(s2), std::move(s1), std::move(u)};
      break;
    case MergeOrder::S1US2:
      slices = {std::move(s1), std::move(u), std::move(s2)};
      break;
    case MergeOrder::S2US1:
      slices = {std::move(s2), std::move(u), std::move(s1)};
      break;
    case MergeOrder::US2S1:
      slices = {std::move(u), std::move(s2), std::move(s1)};
      break;
    default:
      assert("Invalid MergeOrder!" && false);
    }

    splits = slicePosition != splitChain->nodeBundles.begin();
    needsSplitChainRotation = (mergeOrder == S2S1U && splits) ||
                              mergeOrder == S2US1 || mergeOrder == US2S1;
    needsBundling = splitChain->size + unsplitChain->size > propConfig.optChainSplitThreshold;
    // Set the ExtTSP score gain as the difference between the new score after
    // merging these chains and the current scores of the two chains.
    auto assemblyScore = computeExtTSPScore();
    auto chainsScore = splitChain->score + unsplitChain->score;
    scoreGain = assemblyScore > chainsScore ? assemblyScore - chainsScore : 0;
  }

  bool isValid() { return scoreGain > 0 && (propConfig.optReorderIP || (!splitChain()->firstNode()->isEntryNode() && !unsplitChain()->firstNode()->isEntryNode()) || getFirstNode()->isEntryNode()); }

  // Find the NodeChainSlice in this NodeChainAssembly which contains the given
  // node. If the node is not contained in this NodeChainAssembly, then return
  // false. Otherwise, set idx equal to the index of the corresponding slice and
  // return true.
  bool findSliceIndex(CFGNode *node, NodeChain *chain, int64_t offset,
                      uint8_t &idx) const;

  // This function computes the ExtTSP score for a chain assembly record. This
  // goes over the three bb slices in the assembly record and considers all
  // edges whose source and sink belong to the chains in the assembly record.
  uint64_t computeExtTSPScore() const;

  // First node in the resulting assembled chain.
  CFGNode *getFirstNode() const {
    for (auto &slice : slices)
      if (!slice.isEmpty())
        return (*slice.beginPosition)->nodes.front();
    return nullptr;
  }

  // Comparator for two nodeChainAssemblies. It compare scoreGain and break ties
  // consistently.
  struct CompareNodeChainAssembly {
    bool operator()(const std::unique_ptr<NodeChainAssembly> &a1,
                    const std::unique_ptr<NodeChainAssembly> &a2) const;
  };

  // We delete the copy constructor to make sure NodeChainAssembly is moved
  // rather than copied.
  NodeChainAssembly(NodeChainAssembly &&) = default;
  // copy constructor is implicitly deleted
  // NodeChainAssembly(NodeChainAssembly&) = delete;
  NodeChainAssembly() = delete;

  // This returns a unique value for every different assembly record between two
  // chains. When chainPair is equal, this helps differentiate and compare the
  // two assembly records.
  std::pair<uint8_t, size_t> assemblyStrategy() const {
    return std::make_pair(mergeOrder,
                          (*slicePosition)->delegateNode->symbol->ordinal);
  }

  inline NodeChain *splitChain() const { return chainPair.first; }

  inline NodeChain *unsplitChain() const { return chainPair.second; }
};

std::string toString(NodeChainAssembly &assembly);

} // namespace propeller
} // namespace llvm

#endif
