#include "llvm_propeller_node_chain.h"

namespace devtools_crosstool_autofdo {

NodeChain *GetNodeChain(const CFGNode *n) {
  return n->bundle_ ? n->bundle_->chain_ : nullptr;
}

int64_t GetNodeOffset(const CFGNode *n) {
  CHECK(n->bundle_) << "Chain not initialized for node.";
  return n->bundle_->chain_offset_ + n->bundle_offset_;
}

}  // namespace devtools_crosstool_autofdo
