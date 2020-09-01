//===- PropellerNodeChain.h  ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLD_ELF_PROPELLER_CODE_LAYOUT_NODE_CHAIN_H
#define LLD_ELF_PROPELLER_CODE_LAYOUT_NODE_CHAIN_H

#include "PropellerCFG.h"
#include "llvm/ADT/DenseSet.h"

#include <list>
#include <unordered_map>
#include <vector>

using llvm::DenseSet;

namespace llvm {
namespace propeller {

class NodeChain;

class CFGNodeBundle {
public:
  CFGNode *delegateNode;
  std::vector<CFGNode *> nodes;

  NodeChain *chain;

  int64_t chainOffset;

  // Total binary size of the chain.
  uint64_t size;

  // Total execution frequency of the chain.
  uint64_t freq;

  CFGNodeBundle(CFGNode *n, NodeChain *c, int64_t o)
      : delegateNode(n), nodes(1, n), chain(c), chainOffset(o), size(n->symSize),
        freq(n->freq) {
    n->bundle = this;
    n->bundleOffset = 0;
  }

  CFGNodeBundle(std::vector<CFGNode *> &ns, NodeChain *c, int64_t o)
      : nodes(ns), chain(c), chainOffset(o), size(0), freq(0) {
    delegateNode = nodes.front();
    for (CFGNode *n : nodes) {
      n->bundle = this;
      n->bundleOffset = size;
      size += n->symSize;
      freq += n->freq;
    }
  }

  void merge(CFGNodeBundle *other) {
    for (CFGNode *n : other->nodes) {
      n->bundle = this;
      n->bundleOffset += size;
    }
    std::move(other->nodes.begin(), other->nodes.end(),
              std::back_inserter(nodes));
    size += other->size;
    freq += other->freq;
  }
};

// Represents a chain of nodes (basic blocks).
class NodeChain {
public:
  // Representative node of the chain, with which it is initially constructed.
  CFGNode *delegateNode;

  // controlFlowGraph of the nodes in this chain (this will be null if the nodes
  // come from more than one cfg).
  ControlFlowGraph *controlFlowGraph;

  // Ordered list of the nodes in this chain.
  std::list<std::unique_ptr<CFGNodeBundle>> nodeBundles;

  // Out edges for this chain, to its own nodes and nodes of other chains.
  std::unordered_map<NodeChain *, std::vector<CFGEdge *>> outEdges;

  // chains which have outgoing edges to this chain.
  DenseSet<NodeChain *> inEdges;

  // Total binary size of the chain.
  uint64_t size;

  // Total execution frequency of the chain.
  uint64_t freq;

  // Extended TSP score of the chain.
  uint64_t score = 0;

  // uint64_t bundleScore = 0;

  // Whether to print out information about how this chain joins with others.
  bool debugChain;

  bool bundled = false;

  // Constructor for building a NodeChain from a single Node
  NodeChain(CFGNode *node) {
    nodeBundles.emplace_back(new CFGNodeBundle(node, this, 0));
    delegateNode = node;
    controlFlowGraph = node->controlFlowGraph;
    size = node->symSize;
    freq = node->freq;
    debugChain = node->controlFlowGraph->debugCFG;
  }

  NodeChain(std::vector<CFGNode *> &nodes) {
    nodeBundles.emplace_back(new CFGNodeBundle(nodes, this, 0));
    delegateNode = nodes.front();
    controlFlowGraph = delegateNode->controlFlowGraph;
    size = nodeBundles.front()->size;
    freq = nodeBundles.front()->size;
    debugChain = delegateNode->controlFlowGraph->debugCFG;
  }

  // Constructor for building a NodeChain from all nodes in the controlFlowGraph
  // according to the initial order.
  NodeChain(ControlFlowGraph *cfg)
      : delegateNode(cfg->getEntryNode()), controlFlowGraph(cfg),
        size(cfg->size), freq(0), debugChain(cfg->debugCFG) {
    int64_t offset = 0;
    cfg->forEachNodeRef([this, &offset](CFGNode &node) {
      nodeBundles.emplace_back(new CFGNodeBundle(&node, this, offset));
      freq += node.freq;
      offset += node.symSize;
    });
  }

  // Helper function to iterate over the outgoing edges of this chain to a
  // specific chain, while applying a given function on each edge.
  template <class Visitor>
  void forEachOutEdgeToChain(NodeChain *chain, Visitor V) {
    auto it = outEdges.find(chain);
    if (it == outEdges.end())
      return;
    for (CFGEdge *E : it->second)
      V(*E, this, chain);
  }

  // This returns the execution density of the chain.
  double execDensity() const {
    return ((double)freq) / std::max(size, (uint64_t)1);
  }

  bool isSameCFG(const NodeChain &c) {
    return controlFlowGraph && controlFlowGraph == c.controlFlowGraph;
  }

  bool isHot() {
    return freq != 0;
  }

  CFGNode *lastNode() const { return nodeBundles.back()->nodes.back(); }

  CFGNode *firstNode() const { return nodeBundles.front()->nodes.front(); }

  bool bundleNodes(std::list<std::unique_ptr<CFGNodeBundle>>::iterator begin,
                   std::list<std::unique_ptr<CFGNodeBundle>>::iterator end);

  bool bundleNodes();
};

// This returns a string representation of the chain
std::string
toString(const NodeChain &c,
         std::list<std::unique_ptr<CFGNodeBundle>>::const_iterator slicePos);
std::string toString(const NodeChain &c);

NodeChain *getNodeChain(const CFGNode *n);

int64_t getNodeOffset(const CFGNode *n);

} // namespace propeller
} // namespace llvm

namespace std {
// Specialization of std::less for NodeChain, which allows for consistent
// tie-breaking in our Map data structures.
template <> struct less<llvm::propeller::NodeChain *> {
  bool operator()(const llvm::propeller::NodeChain *c1,
                  const llvm::propeller::NodeChain *c2) const {
    return c1->delegateNode->symbol->ordinal < c2->delegateNode->symbol->ordinal;
  }
};

// Specialization of std::less for pair<NodeChain,NodeChain>, which allows for
// consistent tie-breaking in our Map data structures.
template <>
struct less<pair<llvm::propeller::NodeChain *, llvm::propeller::NodeChain *>> {
  bool operator()(
      const pair<llvm::propeller::NodeChain *, llvm::propeller::NodeChain *> p1,
      const pair<llvm::propeller::NodeChain *, llvm::propeller::NodeChain *> p2)
      const {
    if (less<llvm::propeller::NodeChain *>()(p1.first, p2.first))
      return true;
    if (less<llvm::propeller::NodeChain *>()(p2.first, p1.first))
      return false;
    return less<llvm::propeller::NodeChain *>()(p1.second, p2.second);
  }
};
} // namespace std

#endif
