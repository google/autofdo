//===- NodeChain.cpp  -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "NodeChain.h"

namespace llvm {
namespace propeller {

// This merges the bundles between (and including) two bundles in the chain's
// bundle list. Two bundles can be merged together if they are from the same
// function (cfg).
// Returns true if any bundles were merged.
bool NodeChain::bundleNodes(std::list<std::unique_ptr<CFGNodeBundle>>::iterator begin,
                            std::list<std::unique_ptr<CFGNodeBundle>>::iterator end) {
  CFGNodeBundle *bundle = (begin == nodeBundles.begin())
                              ? nullptr
                              : (*std::prev(begin)).get();
  bool changed = false;
  for (auto it = begin; it != end;) {
    if (!bundle || (*it)->delegateNode->controlFlowGraph !=
                       bundle->delegateNode->controlFlowGraph) {
      bundle = (*it).get();
      it++;
    } else {
      changed = true;
      bundle->merge((*it).get());
      it = nodeBundles.erase(it);
    }
  }
  return changed;
}

// This merges all the bundles in the chain's bundle list.
bool NodeChain::bundleNodes() {
  return bundleNodes(nodeBundles.begin(), nodeBundles.end());
}

// Get the chain containing a given node.
NodeChain *getNodeChain(const CFGNode *n) { return n->bundle->chain; }

// Get the offset of a node in its containing chain.
int64_t getNodeOffset(const CFGNode *n) {
  return n->bundle->chainOffset + n->bundleOffset;
}

std::string
toString(const NodeChain &c,
         std::list<std::unique_ptr<CFGNodeBundle>>::const_iterator slicePos) {
  std::string str;
  if (c.controlFlowGraph)
    str += c.controlFlowGraph->name.str();
  str += " {{{ ";
  for (auto bundleIt = c.nodeBundles.begin(); bundleIt != c.nodeBundles.end();
       ++bundleIt) {
    if (bundleIt == slicePos)
      str += "\n....SLICE POSITION....\n";

    str += " << bundle [offset= " + std::to_string((*bundleIt)->chainOffset) +
           " size=" + std::to_string((*bundleIt)->size) +
           " freq=" + std::to_string((*bundleIt)->freq) + " ] ";
    for (auto *n : (*bundleIt)->nodes) {
      if (!c.controlFlowGraph)
        str += std::to_string(n->controlFlowGraph->getEntryNode()->symbol->ordinal) +
               ":";
      str += n->controlFlowGraph->getEntryNode() == n
                 ? "Entry"
                 : std::to_string(n->getFullName().size() -
                                  n->controlFlowGraph->name.size() - 4);
      str += " (size=" + std::to_string(n->symSize) +
             ", freq=" + std::to_string(n->freq) +
             ", offset=" + std::to_string(n->bundleOffset) + ")";
      if (n != (*bundleIt)->nodes.back())
        str += " -> ";
    }
    str += " >> ";
  }
  str += " }}}";
  str += " score: " + std::to_string(c.score);
  return str;
}

std::string toString(const NodeChain &c) {
  return toString(c, c.nodeBundles.end());
}

} // namespace propeller
} // namespace llvm
