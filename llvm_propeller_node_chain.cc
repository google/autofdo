#include "llvm_propeller_node_chain.h"

namespace devtools_crosstool_autofdo {

NodeChain *GetNodeChain(const CFGNode *n) {
  return n->bundle() ? n->bundle()->chain_ : nullptr;
}

int64_t GetNodeOffset(const CFGNode *n) {
  CHECK(n->bundle()) << "Chain not initialized for node.";
  return n->bundle()->chain_offset_;
}

}  // namespace devtools_crosstool_autofdo
