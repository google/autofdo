//===-------------------- PropellerELFCFG.h -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Class definitions for propeller cfg, edge, nodes and CFGBuilder.
//
// The ObjectView class represents one ELF file. The CFGBuilder class builds
// cfg for each function and store it in ObjectView::cfgs, indexed by cfg name.
//
// CFGBuilder::buildCFGs works this way:
//   - groups funcName, a.bb.funcName, aa.bb.funcName and alike into one set,
//     for each set, passes the set to "CFGBuilder::buildCFG"
//   - each element in the set is a section, we then know from its section
//     relocations the connections to other sections. (a)
//   - from (a), we build controlFlowGraph.
//
// Three important functions in ControlFlowGraph:
//   mapBranch - apply counter to edge A->B, where A, B belong to the same func
//
//   mapCallOut - apply counter to edge A->B, where A, B belong to diff funcs
//
//   markPath - apply counter to all nodes/edges betwee A and B, A and B belong
//              to same func
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_PROPELLER_CFG_H
#define LLD_ELF_PROPELLER_CFG_H

//#include "Propeller.h"
#include "CodeLayout/PropellerConfig.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ProfileData/BBSectionsProf.h"

#include <map>
#include <memory>
#include <ostream>
#include <vector>

using llvm::propeller::SymbolEntry;

namespace llvm {
namespace propeller {

class CFGNode;
class ControlFlowGraph;
class CFGNodeBundle;

// All instances of CFGEdge are owned by their controlFlowGraph.
class CFGEdge {
public:
  CFGNode *src;
  CFGNode *sink;
  uint64_t weight;

  // Whether it's an edge introduced by recursive-self-call.  (Usually
  // calls do not split basic blocks and do not introduce new edges.)
  enum EdgeType : char {
    INTRA_FUNC,
    INTRA_RSC, // Recursive call.
    INTRA_RSR, // Return from recursive call.
    // Intra edge dynamically created because of indirect jump, etc.
    INTRA_DYNA,
    // Inter function jumps / calls.
    INTER_FUNC_CALL,
    INTER_FUNC_RETURN,
  } type = INTRA_FUNC;

  bool isCall() const { return type == INTER_FUNC_CALL || type == INTRA_RSC; }

  bool isReturn() const {
    return type == INTER_FUNC_RETURN || type == INTRA_RSR;
  }

  bool isFTEdge() const;

protected:
  CFGEdge(CFGNode *n1, CFGNode *n2, EdgeType t)
      : src(n1), sink(n2), weight(0), type(t) {}

  friend class ControlFlowGraph;
};

// All instances of CFGNode are owned by their controlFlowGraph.
class CFGNode {
public:
  SymbolEntry * symbol;
  uint64_t freq, symSize;
  ControlFlowGraph *controlFlowGraph;

  // Containing bundle for this node assigned by the ordering algorithm, and the
  // offset of the node in that bundle.
  // These two field are updated as bundles get merged together during the
  // algorithm.
  CFGNodeBundle *bundle;
  int64_t bundleOffset;

  std::vector<CFGEdge *> outs;     // Intra function edges.
  std::vector<CFGEdge *> ins;      // Intra function edges.
  std::vector<CFGEdge *> callOuts; // Callouts/returns to other functions.
  std::vector<CFGEdge *> callIns;  // Callins/returns from other functions.

  // Fallthrough edge, could be nullptr. And if not, ftEdge is in outs.
  CFGEdge *ftEdge;

  const static uint64_t InvalidAddress = -1;

  unsigned getBBIndex() const {
    StringRef fName, bName;
    if (!symbol->isFunction())
      return symbol->name.size();
    return 0;
  }

  std::string getFullName() const {
    std::string fullName(symbol->name.str());
    if (!symbol->isFunction()){
      fullName += llvm::propeller::BASIC_BLOCK_SEPARATOR;
      fullName += symbol->containingFunc->name;
    }
    return fullName;
  }

  bool isEntryNode() const;

  template <class Visitor> void forEachInEdgeRef(Visitor v) {
    for (auto &edgeList : {ins, callIns})
      for (CFGEdge *E : edgeList)
        v(*E);
  }

  template <class Visitor> void forEachIntraOutEdgeRef(Visitor v) {
    for (CFGEdge *E : outs)
      v(*E);
  }

  template <class Visitor> void forEachOutEdgeRef(Visitor v) {
    for (auto &edgeList : {outs, callOuts})
      for (CFGEdge *E : edgeList)
        v(*E);
  }

private:
  CFGNode(SymbolEntry * _symbol, ControlFlowGraph *_cfg)
      : symbol(_symbol), controlFlowGraph(_cfg), symSize(_symbol->size), freq(0), bundle(nullptr), bundleOffset(0),
        outs(), ins(), callOuts(), callIns(), ftEdge(nullptr) {}

  friend class ControlFlowGraph;
  friend class CFGBuilder;
};

class ControlFlowGraph {
public:
  StringRef name;
  uint64_t size;

  // Whether propeller should print information about how this controlFlowGraph
  // is being reordered.
  bool debugCFG;
  bool hot;

  // ControlFlowGraph assumes the ownership for all nodes / Edges.
  std::vector<std::unique_ptr<CFGNode>> nodes; // Sorted by address.
  std::vector<std::unique_ptr<CFGEdge>> intraEdges;
  std::vector<std::unique_ptr<CFGEdge>> interEdges;

  std::vector<std::pair<unsigned, std::vector<CFGNode*>>> clusters;

  void calculateNodeFreqs();

  void coalesceColdNodes();

  ControlFlowGraph(const StringRef &n, uint64_t s, std::vector<SymbolEntry*> &symbols)
      : name(n), size(s), hot(false) {
    debugCFG = std::find(propConfig.optDebugSymbols.begin(),
                         propConfig.optDebugSymbols.end(),
                         name.str()) != propConfig.optDebugSymbols.end();
    for (auto* se: symbols) {
      auto *n = new CFGNode(se, this);
      nodes.emplace_back(n);
    }

    auto * entryNode = getEntryNode();
    forEachNodeRef([entryNode](CFGNode &n) {
      if (!n.isEntryNode())
        entryNode->symSize -= n.symSize;
    });
  }

  bool markPath(CFGNode *from, CFGNode *to, uint64_t cnt = 1);
  void mapBranch(CFGNode *from, CFGNode *to, uint64_t cnt = 1,
                 bool isCall = false, bool isReturn = false);
  void mapCallOut(CFGNode *from, CFGNode *to, uint64_t toAddr, uint64_t cnt = 1,
                  bool isCall = false, bool isReturn = false);

  CFGNode *getEntryNode() const {
    assert(!nodes.empty());
    return nodes.begin()->get();
  }

  bool isHot() const {
    if (nodes.empty())
      return false;
    return hot;
  }

  template <class Visitor> void forEachNodeRef(Visitor v) {
    for (auto &N : nodes)
      v(*N);
  }

  bool writeAsDotGraph(StringRef cfgOutName);

  // Create and take ownership.
  CFGEdge *createEdge(CFGNode *from, CFGNode *to,
                      typename CFGEdge::EdgeType type);

private:
  void emplaceEdge(CFGEdge *edge) {
    if (edge->type < CFGEdge::INTER_FUNC_CALL)
      intraEdges.emplace_back(edge);
    else
      interEdges.emplace_back(edge);
  }
};

std::ostream &operator<<(std::ostream &out, const CFGNode &node);
std::ostream &operator<<(std::ostream &out, const CFGEdge &edge);
std::ostream &operator<<(std::ostream &out, const ControlFlowGraph &cfg);
} // namespace propeller
} // namespace llvm
#endif
