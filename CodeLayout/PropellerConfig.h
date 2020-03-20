//===-------------------- PropellerConfig.h -------------------------------===//
//

#ifndef LLD_ELF_PROPELLER_CONFIG_H
#define LLD_ELF_PROPELLER_CONFIG_H

#include "llvm/ADT/StringRef.h"
using llvm::StringRef;

#include <string>
#include <vector>

namespace llvm {
namespace propeller {

struct PropellerConfig {
  uint64_t optBackwardJumpDistance;
  uint64_t optBackwardJumpWeight = 1;
  uint64_t optChainSplitThreshold = 1024;
  std::vector<llvm::StringRef> optDebugSymbols;
  std::vector<llvm::StringRef> optDumpCfgs;
  uint64_t optClusterMergeSizeThreshold = 1 << 21;
  StringRef optDumpSymbolOrder;
  uint64_t optFallthroughWeight = 10;
  uint64_t optForwardJumpDistance;
  uint64_t optForwardJumpWeight = 1;
  bool optKeepNamedSymbols;
  std::vector<llvm::StringRef> optOpts;
  bool optPrintStats;
  StringRef optPropeller;
  bool optReorderBlocks;
  bool optReorderFuncs;
  bool optSplitFuncs;
  bool optReorderIP;

  PropellerConfig();
};

extern PropellerConfig propConfig;

} // namespace propeller
} // namespace llvm

#endif
