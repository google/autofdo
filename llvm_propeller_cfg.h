#ifndef AUTOFDO_LLVM_PROPELLER_CFG_H_
#define AUTOFDO_LLVM_PROPELLER_CFG_H_

#include <cstdio>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "base/logging.h"  // For "CHECK".
#include "llvm_propeller_bbsections.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {

class CFGNode;
class ControlFlowGraph;
class CFGNodeBundle;

// All instances of CFGEdge are owned by their cfg_.
class CFGEdge {
 public:
  CFGNode *src_;
  CFGNode *sink_;
  uint64_t weight_ = 0;

  // Encoding of edge information.
  enum Info {
    DEFAULT,
    CALL,
    RET,
  } info_ = Info::DEFAULT;

  bool IsCall() const { return info_ == Info::CALL; }

  bool IsReturn() const { return info_ == Info::RET; }

  // TODO(rahmanl): implement this. Ref: b/154263650
  bool IsFallthroughEdge() const;

 protected:
  CFGEdge(CFGNode *n1, CFGNode *n2, uint64_t weight, Info inf)
      : src_(n1), sink_(n2), weight_(weight), info_(inf) {}

  friend class ControlFlowGraph;
};

// All instances of CFGNode are owned by their cfg_.
class CFGNode {
 public:
  uint64_t symbol_ordinal_ = 0;
  uint64_t addr_ = 0;
  // Zero-based index of the basic block in the function. A zero value indicates
  // the entry basic block.
  uint32_t bb_index_ = 0;
  uint64_t freq_ = 0;
  uint64_t size_ = 0;
  ControlFlowGraph *cfg_;

  std::vector<CFGEdge *> intra_outs_ = {};  // Intra function edges.
  std::vector<CFGEdge *> intra_ins_ = {};   // Intra function edges.
  std::vector<CFGEdge *> inter_outs_ = {};  // Calls to other functions.
  std::vector<CFGEdge *> inter_ins_ = {};   // Returns from other functions.

  // "fallthrough_edge_" could be nullptr. And if not, fallthrough_edge_ is also
  // in intra_outs_.
  CFGEdge *fallthrough_edge_ = nullptr;

  CFGNodeBundle *bundle_ = nullptr;
  int64_t bundle_offset_ = 0;

  template <class Visitor>
  void ForEachInEdgeRef(Visitor v) {
    for (auto &edgeList : {intra_ins_, inter_ins_})
      for (CFGEdge *E : edgeList) v(*E);
  }

  template <class Visitor>
  void ForEachOutEdgeRef(Visitor v) {
    for (auto &edgeList : {intra_outs_, inter_outs_})
      for (CFGEdge *E : edgeList) v(*E);
  }

  explicit CFGNode(ControlFlowGraph *cfg) : cfg_(cfg) {}

  bool is_entry() const { return bb_index_ == 0; }

  std::string GetName() const;

 protected:
  CFGNode(SymbolEntry *symbol, ControlFlowGraph *cfg, int bb_index)
      : symbol_ordinal_(symbol->ordinal),
        addr_(symbol->addr),
        bb_index_(bb_index),
        size_(symbol->size),
        cfg_(cfg) {}

  friend class ControlFlowGraph;
};

struct CFGNodeUniquePtrLessComparator {
  bool operator()(const std::unique_ptr<CFGNode> &a,
                  const std::unique_ptr<CFGNode> &b) const {
    if (!a && b) return true;
    if (a && !b) return false;
    if (!a && !b) return false;
    return a->symbol_ordinal_ < b->symbol_ordinal_;
  }
};

class ControlFlowGraph {
 public:
  bool hot_tag_ = false;
  // Function names associated with this CFG: The first name is the primary
  // function name and the rest are aliases. The primary name is necessary.
  llvm::SmallVector<llvm::StringRef, 3> names_;

  // All cold nodes (and their edges) are coalesced into the lowest-ordinal cold
  // node by CoalesceColdNodes. This remains null if all nodes are hot.
  CFGNode* coalesced_cold_node_ = nullptr;

  struct CFGNodeAddressComp {
    bool operator()(const std::unique_ptr<CFGNode> &p1,
                    const std::unique_ptr<CFGNode> &p2) const {
      return p1->symbol_ordinal_ < p2->symbol_ordinal_;
    }
  };
  // CFGs own all nodes. Nodes here are *strictly* sorted by addresses /
  // ordinals.
  std::set<std::unique_ptr<CFGNode>, CFGNodeUniquePtrLessComparator> nodes_;

  // CFGs own all edges. All edges are owned by their src's CFGs and they
  // appear exactly once in one of the following two fields. The src and sink
  // nodes of each edge contain a pointer to the edge, which means, each edge is
  // recorded exactly twice in Nodes' inter_ins_, inter_outs, intra_ints or
  // intra_out_.
  std::vector<std::unique_ptr<CFGEdge>> intra_edges_;
  std::vector<std::unique_ptr<CFGEdge>> inter_edges_;

  CFGNode *GetEntryNode() const {
    CHECK(!nodes_.empty());
    return nodes_.begin()->get();
  }

  llvm::StringRef GetPrimaryName() const {
    CHECK(!names_.empty());
    return names_.front();
  }

  bool IsHot() const {
    if (nodes_.empty()) return false;
    return hot_tag_;
  }

  template <class Visitor>
  void ForEachNodeRef(Visitor v) {
    for (auto &N : nodes_) v(*N);
  }

  bool WriteAsDotGraph(llvm::StringRef cfgOutName);

  // Create node and take ownership.
  void CreateNodes(const std::vector<SymbolEntry *> &syms) {
    assert(nodes_.empty());
    // TODO(b/162192070): cleanup after removing bblabels workflow.
    if (syms[0]->func_ptr == syms[0]) {
      // This is for bblabels workflow, the function symbol also serves as the
      // entry basic block, which has bbindex == 0, for other symbols, the
      // bbindex is the size of its name (name == "aaaa" ==> size == 4).
      for (SymbolEntry *s : syms)
        nodes_.emplace(
            new CFGNode(s, this, s->func_ptr == s ? 0 : s->name.size()));
    } else {
      // This is for bbaddressmap workflow, the function symbol is excluded from
      // "syms" vector, all symbols contains in "syms" are basic block symbols.
      // bbindex starts from 0.
      int bbidx = 0;
      for (SymbolEntry *s : syms) nodes_.emplace(new CFGNode(s, this, bbidx++));
    }
  }

  // Create edge and take ownership. Note: the caller must be responsible for
  // not creating duplicated edges.
  CFGEdge *CreateEdge(CFGNode *from, CFGNode *to, uint64_t weight,
                      typename CFGEdge::Info inf);

  // TODO(b/162192070): remove "adjust_entry_node_size" after removing bblabels
  // workflow.
  void FinishCreatingControlFlowGraph(bool adjust_entry_node_size);

 private:
  void ResetEntryNodeSize();

  void CalculateNodeFreqs();

  void CoalesceColdNodes();
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_CFG_H_
