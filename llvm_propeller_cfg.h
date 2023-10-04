#ifndef AUTOFDO_LLVM_PROPELLER_CFG_H_
#define AUTOFDO_LLVM_PROPELLER_CFG_H_

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/log/log.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/types/span.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFTypes.h"

namespace devtools_crosstool_autofdo {

class CFGNode;
class ControlFlowGraph;

// All instances of CFGEdge are owned by their cfg_.
class CFGEdge final {
 public:
  // Branch kind.
  enum class Kind {
    kBranchOrFallthough,
    kCall,
    kRet,
  };

  CFGEdge(CFGNode *n1, CFGNode *n2, int weight, Kind kind, bool inter_section)
      : src_(n1),
        sink_(n2),
        weight_(weight),
        kind_(kind),
        inter_section_(inter_section) {}

  CFGNode *src() const { return src_; }
  CFGNode *sink() const { return sink_; }
  int weight() const { return weight_; }
  Kind kind() const { return kind_; }
  bool inter_section() const { return inter_section_; }

  bool IsBranchOrFallthrough() const {
    return kind_ == Kind::kBranchOrFallthough;
  }
  bool IsCall() const { return kind_ == Kind::kCall; }
  bool IsReturn() const { return kind_ == Kind::kRet; }

  static std::string GetCfgEdgeKindString(Kind kind);

  void IncrementWeight(int increment) { weight_ += increment; }

  // Decrements the weight of this edge by the minimum of `value` and `weight_`.
  // Returns the weight reduction applied.
  int DecrementWeight(int value) {
    int reduction = std::min(value, weight_);
    if (weight_ < value) {
      LOG(ERROR) << absl::StrFormat(
          "Edge weight is lower than value (%lld): %v", value, *this);
    }
    weight_ -= reduction;
    return reduction;
  }

  // Returns a string to be used as the label in the dot format.
  std::string GetDotFormatLabel() const {
    return absl::StrCat(GetDotFormatLabelForEdgeKind(kind_), "#", weight_);
  }

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const CFGEdge &edge);

 private:
  static std::string GetDotFormatLabelForEdgeKind(Kind kind);

  CFGNode *src_ = nullptr;
  CFGNode *sink_ = nullptr;
  int weight_ = 0;
  const Kind kind_;
  // Whether the edge is across functions in different sections.
  bool inter_section_ = false;
};

// All instances of CFGNode are owned by their cfg_.
class CFGNode final {
 public:
  // CFGNode Id unique with a single CFG.
  struct IntraCfgId {
    // Index of the basic block in the original function.
    int bb_index;
    // Clone number of the basic block (zero for an original block).
    int clone_number;

    bool operator==(const IntraCfgId &other) const {
      return bb_index == other.bb_index && clone_number == other.clone_number;
    }
    bool operator!=(const IntraCfgId &other) const { return !(*this == other); }
    template <typename H>
    friend H AbslHashValue(H h, const IntraCfgId &id) {
      return H::combine(std::move(h), id.bb_index, id.clone_number);
    }
    bool operator<(const IntraCfgId &other) const {
      return std::forward_as_tuple(bb_index, clone_number) <
             std::forward_as_tuple(other.bb_index, other.clone_number);
    }
    template <typename Sink>
    friend void AbslStringify(Sink &sink, const IntraCfgId &id) {
      absl::Format(&sink, "[BB index: %d, clone number: %v]", id.bb_index,
                   id.clone_number);
    }
    friend std::ostream &operator<<(std::ostream &os, const IntraCfgId &id) {
      os << absl::StreamFormat("%v", id);
      return os;
    }
  };

  // This struct represents a full intra-cfg identifier for a basic block,
  // combining the fixed bb_id and intra_cfg_id (consisting of bb_index and
  // clone number) of the associated cfg node.
  struct FullIntraCfgId {
    int bb_id;
    CFGNode::IntraCfgId intra_cfg_id;

    bool operator==(const FullIntraCfgId &other) const {
      return bb_id == other.bb_id && intra_cfg_id == other.intra_cfg_id;
    }
    bool operator!=(const FullIntraCfgId &other) const {
      return !(*this == other);
    }
  };

  // CFGNode Id unique across the program.
  struct InterCfgId {
    int function_index;

    IntraCfgId intra_cfg_id;

    bool operator==(const InterCfgId &other) const {
      return function_index == other.function_index &&
             intra_cfg_id == other.intra_cfg_id;
    }
    bool operator!=(const InterCfgId &other) const { return !(*this == other); }
    template <typename H>
    friend H AbslHashValue(H h, const InterCfgId &id) {
      return H::combine(std::move(h), id.function_index, id.intra_cfg_id);
    }
    bool operator<(const InterCfgId &other) const {
      return std::forward_as_tuple(function_index, intra_cfg_id) <
             std::forward_as_tuple(other.function_index, other.intra_cfg_id);
    }
    template <typename Sink>
    friend void AbslStringify(Sink &sink, const InterCfgId &id) {
      absl::Format(&sink, "[function index: %d, %v]", id.function_index,
                   id.intra_cfg_id);
    }
    friend std::ostream &operator<<(std::ostream &os, const InterCfgId &id) {
      os << absl::StreamFormat("%v", id);
      return os;
    }
  };

  CFGNode(uint64_t addr, int bb_index, int bb_id, int size,
          const llvm::object::BBAddrMap::BBEntry::Metadata &metadata,
          int function_index, int freq = 0, int clone_number = 0,
          int node_index = -1)
      : inter_cfg_id_({function_index, {bb_index, clone_number}}),
        bb_id_(bb_id),
        node_index_(clone_number == 0 ? bb_index : node_index),
        addr_(addr),
        size_(size),
        metadata_(metadata),
        freq_(freq) {}

  // Returns a clone of `*this` with the given assigned `clone_number`, but with
  // zero frequency and empty edges.
  std::unique_ptr<CFGNode> Clone(int clone_number, int node_index) const {
    return std::make_unique<CFGNode>(addr_, bb_index(), bb_id_, size_,
                                     metadata_, function_index(), /*freq=*/0,
                                     clone_number, node_index);
  }

  // Returns a program-wide unique id for this node.
  const InterCfgId &inter_cfg_id() const { return inter_cfg_id_; }
  // Returns a cfg-wide unique id for this node.
  const IntraCfgId &intra_cfg_id() const { return inter_cfg_id_.intra_cfg_id; }
  FullIntraCfgId full_intra_cfg_id() const {
    return {.bb_id = bb_id_, .intra_cfg_id = intra_cfg_id()};
  }
  uint64_t addr() const { return addr_; }
  int bb_id() const { return bb_id_; }
  int bb_index() const { return intra_cfg_id().bb_index; }
  int node_index() const { return node_index_; }
  int clone_number() const { return intra_cfg_id().clone_number; }
  bool is_cloned() const { return clone_number() != 0; }
  // Computes and returns the execution frequency of the node based on its
  // edges.
  int CalculateFrequency() const;
  int size() const { return size_; }
  bool is_landing_pad() const { return metadata_.IsEHPad; }
  bool can_fallthrough() const { return metadata_.CanFallThrough; }
  bool has_return() const { return metadata_.HasReturn; }
  bool has_tail_call() const { return metadata_.HasTailCall; }
  bool has_indirect_branch() const { return metadata_.HasIndirectBranch; }
  int function_index() const { return inter_cfg_id_.function_index; }

  const std::vector<CFGEdge *> &intra_outs() const { return intra_outs_; }
  const std::vector<CFGEdge *> &intra_ins() const { return intra_ins_; }
  const std::vector<CFGEdge *> &inter_outs() const { return inter_outs_; }
  const std::vector<CFGEdge *> &inter_ins() const { return inter_ins_; }

  void ForEachInEdgeRef(absl::FunctionRef<void(CFGEdge &edge)> func) const {
    for (CFGEdge *edge : intra_ins_) func(*edge);
    for (CFGEdge *edge : inter_ins_) func(*edge);
  }

  void ForEachOutEdgeRef(absl::FunctionRef<void(CFGEdge &edge)> func) const {
    for (CFGEdge *edge : intra_outs_) func(*edge);
    for (CFGEdge *edge : inter_outs_) func(*edge);
  }

  // Returns if this is the entry of the function.
  bool is_entry() const { return bb_index() == 0; }

  std::string GetName() const;

  // Returns the edge from `*this` to `node` of kind `kind`, or `nullptr` if
  // no such edge exists.
  CFGEdge *GetEdgeTo(const CFGNode &node, CFGEdge::Kind kind) const;

  // Returns if there are any edge from `*this` to `node` of kind `kind`.
  bool HasEdgeTo(const CFGNode &node, CFGEdge::Kind kind) const {
    return GetEdgeTo(node, kind) != nullptr;
  }

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const CFGNode &node);

 private:
  friend class ControlFlowGraph;

  // Returns the bb index as a string to be used in the dot format.
  std::string GetDotFormatLabel() const {
    std::string result = absl::StrCat(bb_id_);
    if (clone_number()) absl::StrAppend(&result, ".", clone_number());
    return result;
  }

  void set_freq(int freq) { freq_ = freq; }

  InterCfgId inter_cfg_id_;
  // Fixed ID of the basic block, as defined by the compiler. Must be unique
  // within each cfg. Will be used in the propeller profile.
  const int bb_id_;
  // Index of the node in its CFG's `nodes()`.
  const int node_index_;
  const int addr_;
  int size_ = 0;
  const llvm::object::BBAddrMap::BBEntry::Metadata metadata_;
  int freq_ = 0;

  std::vector<CFGEdge *> intra_outs_ = {};  // Intra function edges.
  std::vector<CFGEdge *> intra_ins_ = {};   // Intra function edges.
  std::vector<CFGEdge *> inter_outs_ = {};  // Calls to other functions.
  std::vector<CFGEdge *> inter_ins_ = {};   // Returns from other functions.
};

class ControlFlowGraph {
 public:
  // hot basic block stats for a single cfg.
  struct NodeFrequencyStats {
    // Number of hot (non-zero frequency) basic blocks.
    int n_hot_blocks = 0;
    // Number of hot landing pad basic blocks.
    int n_hot_landing_pads = 0;
    // Number of hot blocks with zero size.
    int n_hot_empty_blocks = 0;
  };
  ControlFlowGraph(llvm::StringRef section_name, int function_index,
                   std::optional<llvm::StringRef> module_name,
                   const llvm::SmallVectorImpl<llvm::StringRef> &names)
      : section_name_(section_name),
        function_index_(function_index),
        module_name_(module_name),
        names_(names.begin(), names.end()) {}
  ControlFlowGraph(llvm::StringRef section_name, int function_index,
                   std::optional<llvm::StringRef> module_name,
                   llvm::SmallVectorImpl<llvm::StringRef> &&names)
      : section_name_(section_name),
        function_index_(function_index),
        module_name_(module_name),
        names_(std::move(names)) {}
  ControlFlowGraph(llvm::StringRef section_name, int function_index,
                   std::optional<llvm::StringRef> module_name,
                   const llvm::SmallVectorImpl<llvm::StringRef> &names,
                   std::vector<std::unique_ptr<CFGNode>> nodes)
      : section_name_(section_name),
        function_index_(function_index),
        module_name_(module_name),
        names_(names.begin(), names.end()),
        nodes_(std::move(nodes)) {
    int bb_index = 0;
    for (auto &n : nodes_) {
      CHECK_EQ(n->function_index(), function_index_);
      if (!n->is_cloned()) {
        CHECK_EQ(n->bb_index(), bb_index++);
      } else {
        clones_by_bb_index_[n->bb_index()].push_back(n.get());
        CHECK_EQ(n->clone_number(), clones_by_bb_index_[n->bb_index()].size());
      }
      if (n->is_landing_pad()) ++n_landing_pads_;
    }
  }

  ControlFlowGraph(const ControlFlowGraph &) = delete;
  ControlFlowGraph &operator=(const ControlFlowGraph &) = delete;
  ControlFlowGraph(ControlFlowGraph &&) = default;
  ControlFlowGraph &operator=(ControlFlowGraph &&) = default;

  int n_landing_pads() const { return n_landing_pads_; }

  // Returns if this CFG has any hot landing pads. Has a worst-case linear-time
  // complexity w.r.t the number of nodes.
  int has_hot_landing_pads() const {
    if (n_landing_pads_ == 0) return false;
    for (const auto &node : nodes_) {
      if (!node->is_landing_pad()) continue;
      if (node->CalculateFrequency() != 0) return true;
    }
    return false;
  }

  // Returns if this CFG has any edges. Has a worst-case linear time complexity
  // w.r.t the number of nodes.
  bool is_hot() const {
    if (!inter_edges_.empty() || !intra_edges_.empty()) return true;
    return absl::c_any_of(
        nodes_, [](const auto &node) { return !node->inter_ins().empty(); });
  }

  CFGNode *GetEntryNode() const {
    CHECK(!nodes_.empty());
    return nodes_.front().get();
  }

  const std::optional<llvm::StringRef> &module_name() const {
    return module_name_;
  }

  llvm::StringRef GetPrimaryName() const {
    CHECK(!names_.empty());
    return names_.front();
  }

  void ForEachNodeRef(absl::FunctionRef<void(const CFGNode &)> fn) const {
    for (const auto &node : nodes_) fn(*node);
  }

  // Create edge and take ownership. Note: the caller must be responsible for
  // not creating duplicated edges.
  CFGEdge *CreateEdge(CFGNode *from, CFGNode *to, int weight,
                      CFGEdge::Kind kind, bool inter_section);

  // If an edge already exists from `from` to `to` of kind `kind`, then
  // increments its edge weight by weight. Otherwise, creates the edge.
  CFGEdge *CreateOrUpdateEdge(CFGNode *from, CFGNode *to, int weight,
                              CFGEdge::Kind kind, bool inter_section);

  // Returns the frequencies of nodes in this CFG in a vector, in the same order
  // as in `nodes_`.
  std::vector<int> GetNodeFrequencies() const {
    std::vector<int> node_frequencies;
    node_frequencies.reserve(nodes_.size());
    for (const auto &node : nodes_)
      node_frequencies.push_back(node->CalculateFrequency());
    return node_frequencies;
  }

  llvm::StringRef section_name() const { return section_name_; }

  int function_index() const { return function_index_; }

  CFGNode &GetNodeById(CFGNode::IntraCfgId id) const {
    if (id.clone_number == 0) {
      CHECK_LE(id.bb_index, nodes_.size());
      CFGNode *node = nodes_.at(id.bb_index).get();
      CHECK_NE(node, nullptr);
      CHECK_EQ(node->bb_index(), id.bb_index);
      return *node;
    }
    return *clones_by_bb_index_.at(id.bb_index).at(id.clone_number - 1);
  }

  const llvm::SmallVector<llvm::StringRef, 3> &names() const { return names_; }
  const std::vector<std::unique_ptr<CFGNode>> &nodes() const { return nodes_; }

  const std::vector<std::unique_ptr<CFGEdge>> &intra_edges() const {
    return intra_edges_;
  }

  const std::vector<std::unique_ptr<CFGEdge>> &inter_edges() const {
    return inter_edges_;
  }

  const absl::flat_hash_map<int, std::vector<CFGNode *>> &clones_by_bb_index()
      const {
    return clones_by_bb_index_;
  }

  // Returns a vector of clone nodes (including the original node) for the given
  // `bb_index`, in increasing order of their clone_number.
  std::vector<CFGNode *> GetAllClonesForBbIndex(int bb_index) const {
    CFGNode &original_node = GetNodeById(
        CFGNode::IntraCfgId{.bb_index = bb_index, .clone_number = 0});
    std::vector<CFGNode *> clone_instances(1, &original_node);
    auto it = clones_by_bb_index_.find(bb_index);
    if (it != clones_by_bb_index_.end())
      absl::c_copy(it->second, std::back_inserter(clone_instances));
    return clone_instances;
  }

  const std::vector<std::vector<CFGNode::FullIntraCfgId>> &clone_paths() const {
    return clone_paths_;
  }

  void AddClonePath(std::vector<CFGNode::FullIntraCfgId> clone_path) {
    clone_paths_.push_back(std::move(clone_path));
  }

  // Clones basic blocks along the path `path_to_clone` given path predecessor
  // block `path_pred_bb_index`. Both `path_pred_bb_index` and `path_to_clone`
  // are specified in terms of bb_indices of the original nodes.
  void ClonePath(int path_pred_bb_index, absl::Span<const int> path_to_clone) {
    std::vector<CFGNode::FullIntraCfgId> clone_path;
    clone_path.reserve(path_to_clone.size() + 1);
    clone_path.push_back(
        GetNodeById(CFGNode::IntraCfgId{.bb_index = path_pred_bb_index,
                                        .clone_number = 0})
            .full_intra_cfg_id());

    for (int bb_index : path_to_clone) {
      // Get the next available clone number for `bb_index`.
      auto &clones = clones_by_bb_index_[bb_index];
      // Create and insert the clone node.
      clones.push_back(
          nodes_
              .emplace_back(nodes_.at(bb_index)->Clone(
                  clones.size() + 1, static_cast<int>(nodes_.size())))
              .get());
      clone_path.push_back(clones.back()->full_intra_cfg_id());
      if (clones.back()->is_landing_pad()) ++n_landing_pads_;
    }
    // Add this path to `clone_paths_`.
    clone_paths_.push_back(std::move(clone_path));
  }

  // Writes the dot format of CFG into the given stream. `layout_index_map`
  // specifies a layout by mapping basic block intra_cfg_id to their positions
  // in the layout. Fall-through edges will be colored differently
  // (red) in the dot format. `layout_indexx_map` can be a partial map.
  void WriteDotFormat(std::ostream &os,
                      const absl::flat_hash_map<CFGNode::IntraCfgId, int>
                          &layout_index_map) const;

  // Returns the bb_indexes of hot join nodes in this CFG. These are nodes which
  // have a frequency of at least `hot_node_frequency_threshold` and at least
  // two incoming intra-function edges at least as heavy as
  // hot_edge_frequency_threshold`.
  std::vector<int> GetHotJoinNodes(int hot_node_frequency_threshold,
                                   int hot_edge_frequency_threshold) const;

  NodeFrequencyStats GetNodeFrequencyStats() const;

 private:
  // The output section name for this function within which it can be reordered.
  llvm::StringRef section_name_;

  // Unique index of the function in the SHT_LLVM_BB_ADDR_MAP section.
  int function_index_;

  std::optional<llvm::StringRef> module_name_;

  // Function names associated with this CFG: The first name is the primary
  // function name and the rest are aliases. The primary name is necessary.
  llvm::SmallVector<llvm::StringRef, 3> names_;

  // CFGs own all nodes. Nodes here are *strictly* sorted by addresses /
  // ordinals.
  std::vector<std::unique_ptr<CFGNode>> nodes_;

  // Number of nodes which are exception handling pads.
  int n_landing_pads_ = 0;

  // Cloned CFG nodes mapped by bb_indexes of the original nodes.
  // `clone_number` of each node in this map must be equal to 1 + its index in
  // its vector.
  absl::flat_hash_map<int, std::vector<CFGNode *>> clones_by_bb_index_;

  // Cloned paths starting with their path predecessor block ID.
  std::vector<std::vector<CFGNode::FullIntraCfgId>> clone_paths_;

  // CFGs own all edges. All edges are owned by their src's CFGs and they
  // appear exactly once in one of the following two fields. The src and sink
  // nodes of each edge contain a pointer to the edge, which means, each edge is
  // recorded exactly twice in Nodes' inter_ins_, inter_outs, intra_ints or
  // intra_out_.
  std::vector<std::unique_ptr<CFGEdge>> intra_edges_;
  std::vector<std::unique_ptr<CFGEdge>> inter_edges_;
};

template <typename Sink>
inline void AbslStringify(Sink &sink, const CFGEdge &edge) {
  absl::Format(&sink, "[%s -> %s, weight(%lld), type(%s), inter-section(%d)]",
               edge.src()->GetName(), edge.sink()->GetName(), edge.weight(),
               CFGEdge::GetCfgEdgeKindString(edge.kind()),
               edge.inter_section());
}

template <typename Sink>
inline void AbslStringify(Sink &sink, const CFGNode &node) {
  absl::Format(&sink, "[id: %v, addr:%llu size: %d]", node.inter_cfg_id_,
               node.addr_, node.size_);
}

std::ostream &operator<<(std::ostream &os, const CFGEdge::Kind &kind);

// Returns a clone of `cfg` with its nodes and intra-function edges cloned and
// its inter-function edges dropped.
std::unique_ptr<devtools_crosstool_autofdo::ControlFlowGraph> CloneCfg(
    const devtools_crosstool_autofdo::ControlFlowGraph &cfg);
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_CFG_H_
