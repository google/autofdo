#include "llvm_propeller_node_chain.h"

namespace devtools_crosstool_autofdo {

NodeChain &GetNodeChain(const CFGNode *n) {
  CHECK_NE(n->bundle(), nullptr);
  CHECK_NE(n->bundle()->chain_, nullptr);
  return *n->bundle()->chain_;
}

int64_t GetNodeOffset(const CFGNode *n) {
  CHECK(n->bundle()) << "Chain not initialized for node.";
  return n->bundle()->chain_offset_;
}

}  // namespace devtools_crosstool_autofdo
