#ifndef AUTOFDO_LLVM_PROPELLER_CFG_H_
#define AUTOFDO_LLVM_PROPELLER_CFG_H_

#include <cstdio>
#include <memory>
#include <numeric>
#include <ostream>
#include <set>
#include <string>
#include <utility>
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
class CFGEdge final {
 public:
  // Branch kind.
  enum class Kind {
    kBranchOrFallthough,
    kCall,
    kRet,
  };

  CFGEdge(CFGNode *n1, CFGNode *n2, uint64_t weight, Kind kind)
      : src_(n1), sink_(n2), weight_(weight), kind_(kind) {}

  CFGNode * src() const { return src_ ;}
  CFGNode *sink() const { return sink_; }
  uint64_t weight() const { return weight_; }
  Kind kind() const { return kind_; }

  bool IsFallthrough() const { return kind_ == Kind::kBranchOrFallthough; }
  bool IsCall() const { return kind_ == Kind::kCall; }
  bool IsReturn() const { return kind_ == Kind::kRet; }

  // TODO(rahmanl): implement this. Ref: b/154263650
  bool IsFallthroughEdge() const;

 private:
  friend class ControlFlowGraph;
  friend class PropellerWholeProgramInfo;

  void IncrementWeight(uint64_t increment) { weight_ += increment; }
  void ReplaceSink(CFGNode *sink) { sink_ = sink; }
  CFGNode *src_ = nullptr;
  CFGNode *sink_ = nullptr;
  uint64_t weight_ = 0;
  const Kind kind_;
};

std::ostream& operator<<(std::ostream& os, CFGEdge::Kind kind);

// All instances of CFGNode are owned by their cfg_.
class CFGNode final {
 public:
  explicit CFGNode(uint64_t symbol_ordinal, uint64_t addr, uint64_t bb_index,
                   uint64_t size, ControlFlowGraph *cfg, uint64_t freq = 0)
      : symbol_ordinal_(symbol_ordinal),
        addr_(addr),
        bb_index_(bb_index),
        freq_(freq),
        size_(size),
        cfg_(cfg) {}

  uint64_t symbol_ordinal() const { return symbol_ordinal_; }
  uint64_t addr() const { return addr_; }
  uint32_t bb_index() const { return bb_index_; }
  uint64_t freq() const { return freq_; }
  uint64_t size() const { return size_ ;}
  ControlFlowGraph* cfg() const { return cfg_; }
  CFGNodeBundle *bundle() const { return bundle_; }

  const std::vector<CFGEdge *> &intra_outs() const { return intra_outs_; }
  const std::vector<CFGEdge *> &intra_ins() const { return intra_ins_; }
  const std::vector<CFGEdge *> &inter_outs() const { return inter_outs_; }
  const std::vector<CFGEdge *> &inter_ins() const { return inter_ins_; }

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
  bool is_entry() const { return bb_index_ == 0; }

  std::string GetName() const;

 private:
  friend class ControlFlowGraph;
  friend class CFGNodeBundle;

  void GrowOnCoallesce(uint64_t size_increment) { size_ += size_increment; }

  void set_freq(uint64_t freq) { freq_ = freq; }
  void set_bundle(CFGNodeBundle *bundle) {
    DCHECK_EQ(bundle_, nullptr);
    bundle_ = bundle;
  }

  const uint64_t symbol_ordinal_;
  const uint64_t addr_;
  // Zero-based index of the basic block in the function. A zero value indicates
  // the entry basic block.
  const uint32_t bb_index_;
  uint64_t freq_ = 0;
  uint64_t size_ = 0;
  ControlFlowGraph * const cfg_;

  std::vector<CFGEdge *> intra_outs_ = {};  // Intra function edges.
  std::vector<CFGEdge *> intra_ins_ = {};   // Intra function edges.
  std::vector<CFGEdge *> inter_outs_ = {};  // Calls to other functions.
  std::vector<CFGEdge *> inter_ins_ = {};   // Returns from other functions.

  CFGNodeBundle *bundle_ = nullptr;
};

struct CFGNodeUniquePtrLessComparator {
  bool operator()(const std::unique_ptr<CFGNode> &a,
                  const std::unique_ptr<CFGNode> &b) const {
    if (!a && b) return true;
    if (a && !b) return false;
    if (!a && !b) return false;
    return a->symbol_ordinal() < b->symbol_ordinal();
  }
};

class ControlFlowGraph {
 public:
  explicit ControlFlowGraph(const llvm::SmallVectorImpl<llvm::StringRef> &names)
      : names_(names.begin(), names.end()) {}
  explicit ControlFlowGraph(llvm::SmallVectorImpl<llvm::StringRef> &&names)
      : names_(std::move(names)) {}

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
    DCHECK(nodes_.empty());
    CHECK_NE(syms[0]->func_ptr, syms[0]);

    // This is for bbaddressmap workflow, the function symbol is excluded from
    // "syms" vector, all symbols contains in "syms" are basic block symbols.
    // bbindex starts from 0.
    int bbidx = 0;
    for (SymbolEntry *s : syms)
      nodes_.emplace(
          new CFGNode(s->ordinal, s->addr, bbidx++, s->size, this));
  }

  // Create edge and take ownership. Note: the caller must be responsible for
  // not creating duplicated edges.
  CFGEdge *CreateEdge(CFGNode *from, CFGNode *to, uint64_t weight,
                      CFGEdge::Kind kind);

  void FinishCreatingControlFlowGraph();

  const llvm::SmallVectorImpl<llvm::StringRef> &names() const {
    return names_;
  }
  const std::set<std::unique_ptr<CFGNode>, CFGNodeUniquePtrLessComparator>
      &nodes() const {
    return nodes_;
  }

  const std::vector<std::unique_ptr<CFGEdge>> &intra_edges() const {
    return intra_edges_;
  }

  const std::vector<std::unique_ptr<CFGEdge>> &inter_edges() const {
    return inter_edges_;
  }

  // APIs for test purposes.
  static std::unique_ptr<ControlFlowGraph> CreateForTest() {
    return std::make_unique<ControlFlowGraph>(
        llvm::SmallVector<llvm::StringRef, 4>());
  }

  CFGNode *InsertNodeForTest(std::unique_ptr<CFGNode> node) {
    return nodes_.insert(std::move(node)).first->get();
  }

  const CFGNode* GetCoallescedColdNodeForTest() const {
    return coalesced_cold_node_;
  }

 private:
  friend class MockPropellerWholeProgramInfo;
  friend class PropellerProfWriter;

  void CalculateNodeFreqs();

  void CoalesceColdNodes();

  bool hot_tag_ = false;
  // Function names associated with this CFG: The first name is the primary
  // function name and the rest are aliases. The primary name is necessary.
  llvm::SmallVector<llvm::StringRef, 3> names_;

  // All cold nodes (and their edges) are coalesced into the lowest-ordinal cold
  // node by CoalesceColdNodes. This remains null if all nodes are hot.
  CFGNode* coalesced_cold_node_ = nullptr;

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
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_CFG_H_
