//===- CodeLayout.cpp  ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This file is part of the Propeller infrastructure for doing code layout
// optimization and implements the entry point of code layout optimization
// (doSplitOrder).
//===----------------------------------------------------------------------===//
#include "CodeLayout.h"

#include "NodeChain.h"
#include "NodeChainBuilder.h"
#include "NodeChainClustering.h"
#include "PropellerCFG.h"
#include "PropellerConfig.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Twine.h"

#include <chrono>
#include <vector>

using llvm::DenseMap;
using llvm::Twine;

namespace llvm {
namespace propeller {

PropellerConfig propConfig;
extern uint64_t getEdgeExtTSPScore(const CFGEdge &edge,
                                   int64_t srcSinkDistance);

// This function iterates over the cfgs included in the Propeller profile and
// adds them to cold and hot cfg lists. Then it appropriately performs basic
// block reordering by calling NodeChainBuilder.doOrder() either on all cfgs (if
// -propeller-opt=reorder-ip) or individually on every controlFlowGraph. After
// creating all the node chains, it hands the basic block chains to a
// ChainClustering instance for further rerodering.
void CodeLayout::doOrder(std::map<SymbolEntry*, std::unique_ptr<ControlFlowGraph>> &cfgs,
                         std::list<CFGNode*> &section_order) {
  propConfig = PropellerConfig();
  std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();

  // Populate the hot and cold cfg lists by iterating over the cfgs in the
  // propeller profile.
  //prop->forEachCfgRef([this](ControlFlowGraph &cfg) {
  for(auto &cfg_elem: cfgs) {
    auto& cfg = *cfg_elem.second.get();
    if (cfg.isHot()) {
      hot_cfgs.push_back(&cfg);
      if (propConfig.optPrintStats) {
        // Dump the number of basic blocks and hot basic blocks for every
        // function
        unsigned hot_bbs = 0;
        unsigned all_bbs = 0;
        cfg.forEachNodeRef([&hot_bbs, &all_bbs](CFGNode &node) {
          if (node.freq)
            hot_bbs++;
          all_bbs++;
        });
        fprintf(stderr, "HISTOGRAM: %s,%u,%u\n", cfg.name.str().c_str(), all_bbs,
                hot_bbs);
      }
    } else
      cold_cfgs.push_back(&cfg);
  }

  fprintf(stderr, "Hot cfgs: %d\n", hot_cfgs.size());

  if (propConfig.optReorderIP || propConfig.optReorderFuncs)
    clustering.reset(new CallChainClustering());
  else {
    // If function ordering is disabled, we want to conform the the initial
    // ordering of functions in both the hot and the cold layout.
    clustering.reset(new NoOrdering());
  }

  if (propConfig.optReorderIP) {
    // If -propeller-opt=reorder-ip we want to run basic block reordering on all
    // the basic blocks of the hot cfgs.
    NodeChainBuilder(hot_cfgs).doOrder(clustering);
  } else if (propConfig.optReorderBlocks) {
    // Otherwise we apply reordering on every controlFlowGraph separately
    for (ControlFlowGraph *cfg : hot_cfgs)
      NodeChainBuilder(cfg).doOrder(clustering);
  } else { // Reordering of basic blocks is not desired.
    for (ControlFlowGraph *cfg : hot_cfgs) {
      // With function splitting, we just split the hot and cold parts of each
      // function and keep the basic blocks in the original order.
      if (propConfig.optSplitFuncs) {
        std::vector<CFGNode *> cold_nodes, hot_nodes;
        cfg->forEachNodeRef([&cold_nodes, &hot_nodes](CFGNode &n) {
          if (n.freq)
            hot_nodes.emplace_back(&n);
          else
            cold_nodes.emplace_back(&n);
        });
        auto compareNodeAddress = [](CFGNode *n1, CFGNode *n2) {
          return n1->symbol->ordinal < n2->symbol->ordinal;
        };
        std::sort(hot_nodes.begin(), hot_nodes.end(), compareNodeAddress);
        std::sort(cold_nodes.begin(), cold_nodes.end(), compareNodeAddress);
        if (!hot_nodes.empty())
          clustering->addChain(
              std::unique_ptr<NodeChain>(new NodeChain(hot_nodes)));
        if (!cold_nodes.empty())
          clustering->addChain(
              std::unique_ptr<NodeChain>(new NodeChain(cold_nodes)));
      } else {
        // If no-reordering is not desired, we layout out basic blocks according
        // to the original order.
        clustering->addChain(std::unique_ptr<NodeChain>(new NodeChain(cfg)));
      }
    }
  }

  // The order for cold cfgs remains unchanged.
  for (ControlFlowGraph *cfg : cold_cfgs)
    clustering->addChain(std::unique_ptr<NodeChain>(new NodeChain(cfg)));

  // After building all the chains, let the chain clustering algorithm perform
  // the final reordering and populate the hot and cold cfg node orders.
  clustering->doOrder(hot_order, cold_order);

  // Transfter the order to the symbol list.
  ControlFlowGraph * cfg = nullptr;
  unsigned layout_index = 0;
  std::vector<unsigned> cluster;
  for (CFGNode *n : hot_order) {
    if (cfg != n->controlFlowGraph || n->isEntryNode())
      (cfg = n->controlFlowGraph)->clusters.emplace_back();
    if (cfg->clusters.back().second.empty()) {
      section_order.push_back(n);
      cfg->clusters.back().first = layout_index++;
    }
    cfg->clusters.back().second.push_back(n);
  }

  for (CFGNode *n : cold_order)
    section_order.push_back(n);

  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  fprintf(stderr, "[Propeller]: bb reordering took: %d",
       Twine(std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                 .count()));

  if (propConfig.optPrintStats)
    printStats();
}

// Prints statistics of the computed layout, including the number of partitions
// for each function across the code layout, the edge count for each distance
// level, and the ExtTSP score achieved for each function.
void CodeLayout::printStats() {
  DenseMap<CFGNode *, int64_t> nodeAddressMap;
  llvm::StringMap<unsigned> functionPartitions;
  int64_t currentAddress = 0;
  ControlFlowGraph *currentCFG = nullptr;
  for (CFGNode *n : hot_order) {
    if (currentCFG != n->controlFlowGraph)
      functionPartitions[(currentCFG = n->controlFlowGraph)->name]++;
    nodeAddressMap[n] = currentAddress;
    currentAddress += n->symSize;
  }

  for (auto &elem : functionPartitions)
    fprintf(stderr, "FUNCTION PARTITIONS: %s,%u\n", elem.first().str().c_str(),
            elem.second);

  std::vector<uint64_t> distances({0, 128, 640, 1028, 4096, 65536, 2 << 20,
                                   std::numeric_limits<uint64_t>::max()});
  std::map<uint64_t, uint64_t> histogram;
  llvm::StringMap<uint64_t> extTSPScoreMap;
  for (CFGNode *n : hot_order) {
    auto scoreEntry =
        extTSPScoreMap.try_emplace(n->controlFlowGraph->name, 0).first;
    n->forEachOutEdgeRef(
        [&nodeAddressMap, &distances, &histogram, &scoreEntry](CFGEdge &edge) {
          if (!edge.weight || edge.isReturn())
            return;
          if (nodeAddressMap.find(edge.src) == nodeAddressMap.end() ||
              nodeAddressMap.find(edge.sink) == nodeAddressMap.end()) {
            fprintf(stderr, "Found a hot edge whose source and sink do not show up in the "
                 "layout!");
            return;
          }
          int64_t srcOffset = nodeAddressMap[edge.src];
          int64_t sinkOffset = nodeAddressMap[edge.sink];
          int64_t srcSinkDistance = sinkOffset - srcOffset - edge.src->symSize;

          if (edge.type == CFGEdge::EdgeType::INTRA_FUNC ||
              edge.type == CFGEdge::EdgeType::INTRA_DYNA)
            scoreEntry->second += getEdgeExtTSPScore(edge, srcSinkDistance);

          auto res = std::lower_bound(distances.begin(), distances.end(),
                                      std::abs(srcSinkDistance));
          histogram[*res] += edge.weight;
        });
  }

  for (auto &elem : extTSPScoreMap)
    fprintf(stderr, "Ext TSP score: %s %lu\n", elem.first().str().c_str(),
            elem.second);
  fprintf(stderr, "DISTANCE HISTOGRAM: ");
  uint64_t sumEdgeWeights = 0;
  for (auto elem : histogram)
    sumEdgeWeights += elem.second;
  for (auto elem : histogram)
    fprintf(stderr, "\t[%lu -> %lu (%.2f%%)]", elem.first, elem.second,
            (double)elem.second * 100 / sumEdgeWeights);
  fprintf(stderr, "\n");
}

} // namespace propeller
} // namespace llvm
