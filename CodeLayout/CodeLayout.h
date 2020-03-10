//===- PropellerBBReordering.h  -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLD_ELF_PROPELLER_BB_REORDERING_H
#define LLD_ELF_PROPELLER_BB_REORDERING_H

#include "NodeChainClustering.h"
#include "PropellerCFG.h"
#include "PropellerConfig.h"

#include <list>
#include <vector>

namespace lld {
namespace propeller {


class CodeLayout {
private:
  // cfgs that are processed by the reordering algorithm. These are separated
  // into hot and cold cfgs.
  std::vector<ControlFlowGraph *> hotCFGs, coldCFGs;
  // The final hot and cold order containing all cfg nodes.
  std::vector<CFGNode *> HotOrder, ColdOrder;
  // Handle of the clustering algorithm used to further reorder the computed
  // chains.
  std::unique_ptr<ChainClustering> clustering;

public:
  void doSplitOrder(std::map<StringRef, std::unique_ptr<ControlFlowGraph>> &cfgs,
                    std::list<std::string> &symbolList);

  void printStats();
};

} // namespace propeller
} // namespace lld
#endif
