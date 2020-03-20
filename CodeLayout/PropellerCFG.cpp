//===-------------------- PropellerELFCfg.cpp -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file creates cfg and maps propeller profile onto cfg nodes / edges.
//
//===----------------------------------------------------------------------===//
#include "PropellerCFG.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <memory>
#include <numeric>
#include <ostream>
#include <stdio.h>
#include <string>
#include <vector>

using llvm::SmallVector;
using llvm::Twine;

namespace llvm {
namespace propeller {

bool CFGNode::isEntryNode() const {
  return controlFlowGraph->getEntryNode() == this;
}

bool ControlFlowGraph::writeAsDotGraph(StringRef cfgOutName) {
  std::error_code ec;
  llvm::raw_fd_ostream os(cfgOutName, ec, llvm::sys::fs::CD_CreateAlways);
  if (ec.value()) {
    fprintf(stderr, "failed to open: '%s'", cfgOutName.str().c_str());
    return false;
  }
  os << "digraph " << name.str() << "{\n";
  forEachNodeRef([&os](CFGNode &n) {
    os << n.getBBIndex() << " [size=\"" << n.symbol->size << "\"];";
  });
  os << "\n";
  for (auto &e : intraEdges) {
    bool IsFTEdge = (e->src->ftEdge == e.get());
    os << " " << e->src->getBBIndex() << " -> " << e->sink->getBBIndex()
       << " [label=\"" << e->weight
       << "\", weight=" << (IsFTEdge ? "1.0" : "0.1") << "];\n";
  }
  os << "}\n";
  llvm::outs() << "done dumping cfg '" << name.str() << "' into '"
               << cfgOutName.str() << "'\n";
  return true;
}

void ControlFlowGraph::calculateNodeFreqs() {
  auto sumEdgeWeights = [](std::vector<CFGEdge *> &edges) -> uint64_t {
    return std::accumulate(
        edges.begin(), edges.end(), 0,
        [](uint64_t pSum, const CFGEdge *edge) { return pSum + edge->weight; });
  };
  auto ZeroOutEdgeWeights = [](std::vector<CFGEdge *> &Es) {
    for (auto *E : Es)
      E->weight = 0;
  };

  if (nodes.empty())
    return;
  forEachNodeRef([this, &sumEdgeWeights,
                         &ZeroOutEdgeWeights](CFGNode &node) {
      uint64_t maxCallOut =
          node.callOuts.empty()
              ? 0
              : (*std::max_element(node.callOuts.begin(), node.callOuts.end(),
                                   [](const CFGEdge *e1, const CFGEdge *e2) {
                                     return e1->weight < e2->weight;
                                   }))
                    ->weight;
      if (node.symbol->hotTag)
        node.freq =
            std::max({sumEdgeWeights(node.outs), sumEdgeWeights(node.ins),
                      sumEdgeWeights(node.callIns), maxCallOut});
      else {
        node.freq = 0;
        ZeroOutEdgeWeights(node.ins);
        ZeroOutEdgeWeights(node.outs);
        ZeroOutEdgeWeights(node.callIns);
        ZeroOutEdgeWeights(node.callOuts);
      }

      this->hot |= (node.freq != 0);

      // Find non-zero frequency nodes with fallthroughs and propagate the
      // weight via the fallthrough edge if no other normal edge carries weight.
      //if (node.freq && node.ftEdge && node.ftEdge->sink->hotTag) {
      //  uint64_t sumIntraOut = 0;
      //  for (auto *e : node.outs) {
      //    if (e->type == CFGEdge::EdgeType::INTRA_FUNC)
      //      sumIntraOut += e->weight;
      //  }
      //  if (!sumIntraOut)
      //    node.ftEdge->weight = node.freq;
      //}
    });
}

void ControlFlowGraph::coalesceColdNodes() {
  CFGNode * firstColdNode = nullptr;
  nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&firstColdNode] (std::unique_ptr<CFGNode>& n) {
    if (!n->freq){
      if (firstColdNode) {
        firstColdNode->symSize += n->symSize;
        return true;
      } else
        firstColdNode = n.get();
    }
    return false;
  }), nodes.end());
}

// Create an edge for "from->to".
CFGEdge *ControlFlowGraph::createEdge(CFGNode *from, CFGNode *to,
                                      typename CFGEdge::EdgeType type) {
  CFGEdge *edge = nullptr;
  auto CheckExistingEdge = [from, to, type,
                            &edge](std::vector<CFGEdge *> &Edges) {
    for (auto *E : Edges) {
      if (E->src == from && E->sink == to && E->type == type) {
        edge = E;
        return true;
      }
    }
    return false;
  };
  //if (true || !from->hotTag || !to->hotTag) {
    if (type < CFGEdge::EdgeType::INTER_FUNC_CALL &&
        CheckExistingEdge(from->outs))
      return edge;
    if (type >= CFGEdge::EdgeType::INTER_FUNC_CALL &&
        CheckExistingEdge(from->callOuts))
      return edge;
  //}

  edge = new CFGEdge(from, to, type);
  if (type < CFGEdge::EdgeType::INTER_FUNC_CALL) {
    from->outs.push_back(edge);
    to->ins.push_back(edge);
  } else {
    from->callOuts.push_back(edge);
    to->callIns.push_back(edge);
  }
  // Take ownership of "edge", cfg is responsible for all edges.
  emplaceEdge(edge);
  return edge;
}

// Apply counter (cnt) to all edges between node from -> to. Both nodes are from
// the same cfg.
bool ControlFlowGraph::markPath(CFGNode *from, CFGNode *to, uint64_t cnt) {
  assert(from->controlFlowGraph == to->controlFlowGraph);
  if (from == to)
    return true;
  CFGNode *p = from;

  // Iterate over fallthrough edges between from and to, adding every edge in
  // between to a vector.
  SmallVector<CFGEdge *, 32> fallThroughEdges;
  while (p && p != to) {
    if (p->ftEdge) {
      fallThroughEdges.push_back(p->ftEdge);
      p = p->ftEdge->sink;
    } else
      p = nullptr;
  }
  if (!p) // Fallthroughs break between from and to.
    return false;

  for (auto *e : fallThroughEdges)
    e->weight += cnt;

  return true;
}

// Apply counter (cnt) to the edge from node from -> to. Both nodes are from the
// same cfg.
void ControlFlowGraph::mapBranch(CFGNode *from, CFGNode *to, uint64_t cnt,
                                 bool isCall, bool isReturn) {
  assert(from->controlFlowGraph == to->controlFlowGraph);

  for (auto &e : from->outs) {
    bool edgeTypeOk = true;
    if (!isCall && !isReturn)
      edgeTypeOk =
          e->type == CFGEdge::INTRA_FUNC || e->type == CFGEdge::INTRA_DYNA;
    else if (isCall)
      edgeTypeOk = e->type == CFGEdge::INTRA_RSC;
    if (isReturn)
      edgeTypeOk = e->type == CFGEdge::INTRA_RSR;
    if (edgeTypeOk && e->sink == to) {
      e->weight += cnt;
      return;
    }
  }

  CFGEdge::EdgeType type = CFGEdge::INTRA_DYNA;
  if (isCall)
    type = CFGEdge::INTRA_RSC;
  else if (isReturn)
    type = CFGEdge::INTRA_RSR;

  createEdge(from, to, type)->weight += cnt;
}

// Apply counter (cnt) for calls/returns/ that cross function boundaries.
void ControlFlowGraph::mapCallOut(CFGNode *from, CFGNode *to, uint64_t toAddr,
                                  uint64_t cnt, bool isCall, bool isReturn) {
  assert(from->controlFlowGraph == this);
  assert(from->controlFlowGraph != to->controlFlowGraph);
  CFGEdge::EdgeType edgeType = CFGEdge::INTER_FUNC_RETURN;
  if (isCall || (toAddr && to->controlFlowGraph->getEntryNode() == to &&
                 toAddr == to->symbol->ordinal))
    edgeType = CFGEdge::INTER_FUNC_CALL;
  if (isReturn)
    edgeType = CFGEdge::INTER_FUNC_RETURN;
  for (auto &e : from->callOuts)
    if (e->sink == to && e->type == edgeType) {
      e->weight += cnt;
      return;
    }
  createEdge(from, to, edgeType)->weight += cnt;
}

std::ostream &operator<<(std::ostream &out, const CFGNode &node) {
  out << "["
      << (node.getFullName() == node.controlFlowGraph->name
              ? "Entry"
              : std::to_string(node.getFullName().size() -
                               node.controlFlowGraph->name.size() - 4))
      << "]"
      << " [size=" << std::noshowbase << std::dec << node.symbol->size << ", "
      << " addr=" << std::showbase << std::hex << node.symbol->ordinal << ", "
      << " frequency=" << std::showbase << std::dec << node.freq << ", "
      //<< " shndx=" << std::noshowbase << std::dec << node.shndx 
      << "]";
  return out;
}

std::ostream &operator<<(std::ostream &out, const CFGEdge &edge) {
  static const char *TypeStr[] = {"", " (*RSC*)", " (*RSR*)", " (*DYNA*)"};
  out << "edge: " << *edge.src << " -> " << *edge.sink << " [" << std::setw(12)
      << std::setfill('0') << std::noshowbase << std::dec << edge.weight << "]"
      << TypeStr[edge.type];
  return out;
}

std::ostream &operator<<(std::ostream &out, const ControlFlowGraph &cfg) {
  out << "cfg: '" << cfg.name.str()
      << "', size=" << std::noshowbase << std::dec << cfg.size << std::endl;
  for (auto &n : cfg.nodes) {
    auto &node = *n;
    out << "  node: " << node << std::endl;
    for (auto &edge : node.outs) {
      out << "    " << *edge << (edge == node.ftEdge ? " (*FT*)" : "")
          << std::endl;
    }
    for (auto &edge : node.callOuts) {
      out << "    Calls: '" << edge->sink->getFullName()
          << "': " << std::noshowbase << std::dec << edge->weight << std::endl;
    }
  }
  out << std::endl;
  return out;
}

bool CFGEdge::isFTEdge() const { return src->ftEdge == this; }

} // namespace propeller
} // namespace llvm
