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

namespace lld {
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
void CodeLayout::doSplitOrder(std::map<StringRef, std::unique_ptr<ControlFlowGraph>> &cfgs,
                              std::list<std::string> &symbolList,
                              std::unordered_map<SymbolEntry *, std::vector<std::vector<unsigned>>> &bbClusterMap) {
  std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();

  // Populate the hot and cold cfg lists by iterating over the cfgs in the
  // propeller profile.
  //prop->forEachCfgRef([this](ControlFlowGraph &cfg) {
  for(auto &cfgElem: cfgs) {
    auto& cfg = *cfgElem.second.get();
    if (cfg.isHot()) {
      hotCFGs.push_back(&cfg);
      if (propConfig.optPrintStats) {
        // Dump the number of basic blocks and hot basic blocks for every
        // function
        unsigned hotBBs = 0;
        unsigned allBBs = 0;
        cfg.forEachNodeRef([&hotBBs, &allBBs](CFGNode &node) {
          if (node.freq)
            hotBBs++;
          allBBs++;
        });
        fprintf(stderr, "HISTOGRAM: %s,%u,%u\n", cfg.name.str().c_str(), allBBs,
                hotBBs);
      }
    } else
      coldCFGs.push_back(&cfg);
  }

  fprintf(stderr, "Hot cfgs: %d\n", hotCFGs.size());

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
    NodeChainBuilder(hotCFGs).doOrder(clustering);
  } else if (propConfig.optReorderBlocks) {
    // Otherwise we apply reordering on every controlFlowGraph separately
    for (ControlFlowGraph *cfg : hotCFGs) {
      fprintf(stderr, "Ordering for cfg: %s\n", cfg->name.str().c_str());
      NodeChainBuilder(cfg).doOrder(clustering);
    }
  } else {
    // If reordering is not desired, we create changes according to the initial
    // order in the controlFlowGraph.
    for (ControlFlowGraph *cfg : hotCFGs)
      clustering->addChain(std::unique_ptr<NodeChain>(new NodeChain(cfg)));
  }

  // The order for cold cfgs remains unchanged.
  for (ControlFlowGraph *cfg : coldCFGs)
    clustering->addChain(std::unique_ptr<NodeChain>(new NodeChain(cfg)));

  // After building all the chains, let the chain clustering algorithm perform
  // the final reordering and populate the hot and cold cfg node orders.
  clustering->doOrder(HotOrder, ColdOrder);

  std::unordered_map<SymbolEntry *, std::vector<std::vector<unsigned>>>::iterator bbClusterMapIt;
  // Transfter the order to the symbol list.
  ControlFlowGraph * cfg = nullptr;
  std::vector<unsigned> cluster;
  for (CFGNode *n : HotOrder) {
    if (cfg != n->controlFlowGraph || n->isEntryNode()) {
      cfg = n->controlFlowGraph;
      bbClusterMapIt = bbClusterMap.emplace(std::piecewise_construct,
                                       std::forward_as_tuple(cfg->getEntryNode()->symbol),
                                       std::forward_as_tuple()).first;
      bbClusterMapIt->second.emplace_back();
    }
    if (bbClusterMapIt->second.back().empty())
      symbolList.push_back(n->getFullName());
    bbClusterMapIt->second.back().push_back(n->getBBIndex());
  }

  //for (CFGNode *n : ColdOrder)
  //  symbolList.push_back(std::string(n->controlFlowGraph->name.str()) + ".cold");

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
  for (CFGNode *n : HotOrder) {
    if (currentCFG != n->controlFlowGraph) {
      currentCFG = n->controlFlowGraph;
      functionPartitions[currentCFG->name]++;
    }
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
  for (CFGNode *n : HotOrder) {
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
} // namespace lld
