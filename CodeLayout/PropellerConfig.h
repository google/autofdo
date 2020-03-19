//===-------------------- PropellerConfig.h -------------------------------===//
//

#ifndef LLD_ELF_PROPELLER_CONFIG_H
#define LLD_ELF_PROPELLER_CONFIG_H

#include "llvm/ADT/StringRef.h"

using llvm::StringRef;

#include <string>
#include <vector>

namespace lld {
namespace propeller {

struct PropellerConfig {
  uint64_t optBackwardJumpDistance = 680;
  uint64_t optBackwardJumpWeight = 1;
  uint64_t optChainSplitThreshold = 1024;
  std::vector<llvm::StringRef> optDebugSymbols;
  std::vector<llvm::StringRef> optDumpCfgs;
  uint64_t optClusterMergeSizeThreshold = 1 << 21;
  StringRef optDumpSymbolOrder;
  uint64_t optFallthroughWeight = 10;
  uint64_t optForwardJumpDistance = 1024;
  uint64_t optForwardJumpWeight = 1;
  bool optKeepNamedSymbols;
  std::vector<llvm::StringRef> optOpts;
  bool optPrintStats = true;
  StringRef optPropeller;
  bool optReorderBlocks = true;
  bool optReorderFuncs = true;
  bool optSplitFuncs = true;
  bool optReorderIP = true;

  PropellerConfig() {
  // Scale weights for use in the computation of ExtTSP score.
  this->optFallthroughWeight *=
      this->optForwardJumpDistance * this->optBackwardJumpDistance;
  this->optBackwardJumpWeight *= this->optForwardJumpDistance;
  this->optForwardJumpWeight *= this->optBackwardJumpDistance;
  }
};

extern PropellerConfig propConfig;

} // namespace propeller
} // namespace lld

#endif
