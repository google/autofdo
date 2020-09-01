//===- NodeChainAssembly.cpp  ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "NodeChainAssembly.h"
#include "NodeChain.h"

namespace llvm {
namespace propeller {

// Return the Extended TSP score for one edge, given its source to sink
// direction and distance in the layout.
uint64_t getEdgeExtTSPScore(const CFGEdge &edge, int64_t srcSinkDistance) {

  // Approximate callsites to be in the middle of the source basic block.
  if (edge.isCall())
    srcSinkDistance += edge.src->symSize / 2;

  if (edge.isReturn())
    srcSinkDistance += edge.sink->symSize / 2;

  if (srcSinkDistance == 0 && (edge.type == CFGEdge::EdgeType::INTRA_FUNC ||
                               edge.type == CFGEdge::EdgeType::INTRA_DYNA))
    return edge.weight * propConfig.optFallthroughWeight;

  uint64_t absoluteSrcSinkDistance = (uint64_t)std::abs(srcSinkDistance);
  if (srcSinkDistance > 0 &&
      absoluteSrcSinkDistance < propConfig.optForwardJumpDistance)
    return edge.weight * propConfig.optForwardJumpWeight *
           (propConfig.optForwardJumpDistance - absoluteSrcSinkDistance);

  if (srcSinkDistance < 0 &&
      absoluteSrcSinkDistance < propConfig.optBackwardJumpDistance)
    return edge.weight * propConfig.optBackwardJumpWeight *
           (propConfig.optBackwardJumpDistance - absoluteSrcSinkDistance);
  return 0;
}

bool NodeChainAssembly::findSliceIndex(CFGNode *node, NodeChain *chain,
                                       int64_t offset, uint8_t &idx) const {
  for (idx = 0; idx < 3; ++idx) {
    if (chain != slices[idx].chain)
      continue;
    // We find if the node's offset lies within the begin and end offset of this
    // slice.
    if (offset < slices[idx].beginOffset || offset > slices[idx].endOffset)
      continue;
    if (offset < slices[idx].endOffset && offset > slices[idx].beginOffset)
      return true;
    // A node can have zero size, which means multiple nodes may be associated
    // with the same offset. This means that if the node's offset is at the
    // beginning or the end of the slice, the node may reside in either slices
    // of the chain.
    if (offset == slices[idx].endOffset) {
      // If offset is at the end of the slice, iterate backwards over the
      // slice to find a zero-sized node.
      for (auto nodeBundleIt = std::prev(slices[idx].endPosition);
           nodeBundleIt != std::prev(slices[idx].beginPosition);
           nodeBundleIt--) {
        for (auto nodeIt = (*nodeBundleIt)->nodes.rbegin();
             nodeIt != (*nodeBundleIt)->nodes.rend(); ++nodeIt) {
          // Stop iterating if the node's size is non-zero as this would change
          // the offset.
          if ((*nodeIt)->symSize)
            break;
          // Have we found the node?
          if (*nodeIt == node)
            return true;
        }
      }
    }
    if (offset == slices[idx].beginOffset) {
      // If offset is at the beginning of the slice, iterate forwards over the
      // slice to find the node.
      for (auto nodeBundleIt = slices[idx].beginPosition;
           nodeBundleIt != slices[idx].endPosition; nodeBundleIt++) {
        for (auto nodeIt = (*nodeBundleIt)->nodes.begin();
             nodeIt != (*nodeBundleIt)->nodes.end(); ++nodeIt) {
          if (*nodeIt == node)
            return true;
          // Stop iterating if the node's size is non-zero as this would change
          // the offset.
          if ((*nodeIt)->symSize)
            break;
        }
      }
    }
  }
  return false;
}

// This function computes the ExtTSP score for a chain assembly record. This
// goes the three bb slices in the assembly record and considers all edges
// whose source and sink belongs to the chains in the assembly record.
uint64_t NodeChainAssembly::computeExtTSPScore() const {
  // Zero-initialize the score.
  uint64_t score = 0;

  auto addEdgeScore = [this, &score](CFGEdge &edge, NodeChain *srcChain,
                                     NodeChain *sinkChain) {
    uint8_t srcSliceIdx, sinkSliceIdx;
    auto srcNodeOffset = getNodeOffset(edge.src);
    auto sinkNodeOffset = getNodeOffset(edge.sink);
    if (!findSliceIndex(edge.src, srcChain, srcNodeOffset, srcSliceIdx))
      return;

    if (!findSliceIndex(edge.sink, sinkChain, sinkNodeOffset, sinkSliceIdx))
      return;

    int64_t srcSinkDistance = 0;

    if (srcSliceIdx == sinkSliceIdx)
      srcSinkDistance = sinkNodeOffset - srcNodeOffset - edge.src->symSize;
    else {
      bool edgeForward = srcSliceIdx < sinkSliceIdx;
      const NodeChainSlice &srcSlice = slices[srcSliceIdx];
      const NodeChainSlice &sinkSlice = slices[sinkSliceIdx];
      srcSinkDistance =
          edgeForward
              ? srcSlice.endOffset - srcNodeOffset - edge.src->symSize +
                    sinkNodeOffset - sinkSlice.beginOffset
              : srcSlice.beginOffset - srcNodeOffset - edge.src->symSize +
                    sinkNodeOffset - sinkSlice.endOffset;
      // Increment the distance by the size of the middle slice if the src
      // and sink are from the two ends.
      if (srcSliceIdx == 0 && sinkSliceIdx == 2)
        srcSinkDistance += slices[1].size();
      if (srcSliceIdx == 2 && sinkSliceIdx == 0)
        srcSinkDistance -= slices[1].size();
    }

    score += getEdgeExtTSPScore(edge, srcSinkDistance);
  };

  // No changes will be made to the score that is contributed by the unsplit
  // chain and we can simply increment by the chain's stored score.
  score += unsplitChain()->score;

  // We need to recompute the score induced by the split chain (if it has really
  // been split) as the offsets of the nodes have changed.
  if (this->splits)
    splitChain()->forEachOutEdgeToChain(splitChain(), addEdgeScore);
  else
    score += splitChain()->score;

  // Consider the contribution to score for inter-chain edges.
  splitChain()->forEachOutEdgeToChain(unsplitChain(), addEdgeScore);
  unsplitChain()->forEachOutEdgeToChain(splitChain(), addEdgeScore);

  return score;
}

bool NodeChainAssembly::CompareNodeChainAssembly::operator()(
    const std::unique_ptr<NodeChainAssembly> &a1,
    const std::unique_ptr<NodeChainAssembly> &a2) const {

  if (a1->scoreGain == a2->scoreGain) {
    // If score gains are equal, we pick a consistent order based on the chains
    // in the assembly records
    if (std::less<std::pair<NodeChain *, NodeChain *>>()(a1->chainPair,
                                                         a2->chainPair))
      return true;
    if (std::less<std::pair<NodeChain *, NodeChain *>>()(a2->chainPair,
                                                         a1->chainPair))
      return false;
    // When even the chain pairs are the same, we resort to the assembly
    // strategy to pick a consistent order.
    return a1->assemblyStrategy() < a2->assemblyStrategy();
  }
  return a1->scoreGain < a2->scoreGain;
}

static std::string toString(MergeOrder mOrder) {
  switch (mOrder) {
  case MergeOrder::S2S1U:
    return "S2S1U";
  case MergeOrder::S1US2:
    return "S1US2";
  case MergeOrder::S2US1:
    return "S2US1";
  case MergeOrder::US2S1:
    return "US2S1";
  default:
    assert("Invalid MergeOrder!" && false);
    return "";
  }
}

std::string toString(NodeChainAssembly &assembly) {
  std::string str("assembly record between:\n");
  str += toString(*assembly.splitChain(), assembly.slicePosition) + " as S\n";
  str += toString(*assembly.unsplitChain()) + " as U\n";
  str += "merge order: " + toString(assembly.mergeOrder) + "\n";
  str += "scoreGain: " + std::to_string(assembly.scoreGain);
  return str;
}

} // namespace propeller
} // namespace llvm
