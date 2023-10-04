#include "llvm_propeller_cfg.h"

#include <algorithm>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/log/log.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/types/span.h"

namespace devtools_crosstool_autofdo {

std::string CFGNode::GetName() const {
  std::string bb_name = absl::StrCat(function_index());
  if (!is_entry()) absl::StrAppend(&bb_name, ".", bb_index());
  if (clone_number() != 0) absl::StrAppend(&bb_name, ".c", clone_number());
  return bb_name;
}

CFGEdge *CFGNode::GetEdgeTo(const CFGNode &node, CFGEdge::Kind kind) const {
  for (const std::vector<CFGEdge *> *edges : {&intra_outs_, &inter_outs_})
    for (CFGEdge *edge : *edges) {
      if (edge->kind() != kind) continue;
      if (edge->sink() == &node) return edge;
    }
  return nullptr;
}

CFGEdge *ControlFlowGraph::CreateOrUpdateEdge(CFGNode *from, CFGNode *to,
                                              int weight, CFGEdge::Kind kind,
                                              bool inter_section) {
  CFGEdge *edge = from->GetEdgeTo(*to, kind);
  if (edge == nullptr) return CreateEdge(from, to, weight, kind, inter_section);
  edge->IncrementWeight(weight);
  return edge;
}

CFGEdge *ControlFlowGraph::CreateEdge(CFGNode *from, CFGNode *to, int weight,
                                      CFGEdge::Kind kind, bool inter_section) {
  if (inter_section)
    CHECK_NE(from->function_index(), to->function_index())
        << " intra-function edges cannot be inter-section.";
  auto edge = std::make_unique<CFGEdge>(from, to, weight, kind, inter_section);
  auto *ret = edge.get();
  auto has_duplicates = [from,
                         to](absl::Span<const std::unique_ptr<CFGEdge>> edges) {
    for (auto &e : edges)
      if (e->src() == from && e->sink() == to) return true;
    return false;
  };
  (void)(has_duplicates);  // For release build warning.
  if (from->function_index() == to->function_index()) {
    CHECK(!has_duplicates(intra_edges_))
        << " " << from->inter_cfg_id() << " to " << to->inter_cfg_id();
    from->intra_outs_.push_back(edge.get());
    to->intra_ins_.push_back(edge.get());
    intra_edges_.push_back(std::move(edge));
  } else {
    DCHECK(!has_duplicates(inter_edges_));
    from->inter_outs_.push_back(edge.get());
    to->inter_ins_.push_back(edge.get());
    inter_edges_.push_back(std::move(edge));
  }
  return ret;
}

void ControlFlowGraph::WriteDotFormat(
    std::ostream &os,
    const absl::flat_hash_map<CFGNode::IntraCfgId, int> &layout_index_map)
    const {
  os << "digraph {\n";
  os << "label=\"" << GetPrimaryName().str() << "#" << function_index_
     << "\"\n";
  os << "forcelabels=true;\n";
  for (const auto &node : nodes_) {
    os << node->GetDotFormatLabel() << " [xlabel=\"" << node->freq_ << "#"
       << node->size_ << "\", color = \""
       << (node->clone_number() ? "red" : "black") << "\" ];\n";
  }
  for (const auto &edge : intra_edges_) {
    bool is_layout_edge =
        layout_index_map.contains(edge->sink()->intra_cfg_id()) &&
        layout_index_map.contains(edge->src()->intra_cfg_id()) &&
        layout_index_map.at(edge->sink()->intra_cfg_id()) -
                layout_index_map.at(edge->src()->intra_cfg_id()) ==
            1;
    os << edge->src()->GetDotFormatLabel() << " -> "
       << edge->sink()->GetDotFormatLabel() << "[ label=\""
       << edge->GetDotFormatLabel() << "\", color =\""
       << (is_layout_edge ? "red" : "black") << "\"];\n";
  }
  os << "}\n";
}

std::vector<int> ControlFlowGraph::GetHotJoinNodes(
    int hot_node_frequency_threshold, int hot_edge_frequency_threshold) const {
  std::vector<int> ret;
  for (const std::unique_ptr<CFGNode> &node : nodes_) {
    if (node->is_entry()) continue;
    if (node->CalculateFrequency() < hot_node_frequency_threshold) continue;
    auto num_hot_branches_to =
        std::count_if(node->intra_ins().begin(), node->intra_ins().end(),
                      [&](const CFGEdge *edge) {
                        return edge->src() != edge->sink() && !edge->IsCall() &&
                               !edge->IsReturn() &&
                               edge->weight() >= hot_edge_frequency_threshold;
                      });
    if (num_hot_branches_to <= 1) continue;
    ret.push_back(node->bb_index());
  }
  return ret;
}

int CFGNode::CalculateFrequency() const {
  // A node (basic block) may have multiple outgoing calls to different
  // functions. In that case, a single execution of that node counts toward
  // the weight of each of its calls as wells as returns back to the
  // callsites. To avoid double counting, we only consider the heaviest
  // call-out and return-in towards calculating the node's frequency. This
  // mitigates the case discussed in b/155488527 at the expense of possible
  // underestimation. The underestimation may happen when these calls and
  // returns occur in separate LBR stacks. Another source of underestimation
  // is indirect calls. A node may only have one indirect call instruction,
  // but if different functions are called by that indirect call, the node's
  // frequency is equal to the aggregation of call-outs rather than their max.

  int max_call_out = 0;
  int max_ret_in = 0;

  // Total incoming edge frequency to the node's entry (first instruction).
  int sum_in = 0;
  // Total outgoing edge frequency from the node's exit (last instruction).
  int sum_out = 0;

  for (auto *out_edges : {&inter_outs_, &intra_outs_}) {
    for (auto &edge : *out_edges) {
      if (edge->IsCall()) {
        max_call_out = std::max(max_call_out, edge->weight());
      } else {
        sum_out += edge->weight();
      }
    }
  }

  for (auto *in_edges : {&inter_ins_, &intra_ins_}) {
    for (auto &edge : *in_edges) {
      if (edge->IsReturn()) {
        max_ret_in = std::max(max_ret_in, edge->weight());
      } else {
        sum_in += edge->weight();
      }
    }
  }
  return std::max({max_call_out, max_ret_in, sum_out, sum_in});
}

std::string CFGEdge::GetCfgEdgeKindString(Kind kind) {
  switch (kind) {
    case CFGEdge::Kind::kBranchOrFallthough:
      return "BranchOrFallthrough";
    case CFGEdge::Kind::kCall:
      return "Call";
    case CFGEdge::Kind::kRet:
      return "Return";
  }
  LOG(FATAL) << "Invalid edge kind.";
}

std::string CFGEdge::GetDotFormatLabelForEdgeKind(Kind kind) {
  return CFGEdge::GetCfgEdgeKindString(kind).substr(0, 1);
}

std::ostream &operator<<(std::ostream &os, const CFGEdge::Kind &kind) {
  return os << CFGEdge::GetCfgEdgeKindString(kind);
}

std::unique_ptr<ControlFlowGraph> CloneCfg(const ControlFlowGraph &cfg) {
  // Create a clone of `cfg` with all the nodes copied.
  std::vector<std::unique_ptr<CFGNode>> nodes;
  for (const std::unique_ptr<CFGNode> &node : cfg.nodes())
    nodes.push_back(node->Clone(node->clone_number(), nodes.size()));
  auto cfg_clone = std::make_unique<ControlFlowGraph>(
      cfg.section_name(), cfg.function_index(), cfg.module_name(), cfg.names(),
      std::move(nodes));
  // Now copy the intra-function edges.
  for (const std::unique_ptr<CFGEdge> &edge : cfg.intra_edges()) {
    CHECK_EQ(edge->src()->function_index(), edge->sink()->function_index());
    cfg_clone->CreateEdge(&cfg_clone->GetNodeById(edge->src()->intra_cfg_id()),
                          &cfg_clone->GetNodeById(edge->sink()->intra_cfg_id()),
                          edge->weight(), edge->kind(), edge->inter_section());
  }
  return cfg_clone;
}

ControlFlowGraph::NodeFrequencyStats ControlFlowGraph::GetNodeFrequencyStats()
    const {
  ControlFlowGraph::NodeFrequencyStats stats;
  for (const auto &node : nodes_) {
    if (node->CalculateFrequency() == 0) continue;
    ++stats.n_hot_blocks;
    if (node->size() == 0) ++stats.n_hot_empty_blocks;
    if (node->is_landing_pad()) ++stats.n_hot_landing_pads;
  }
  return stats;
}
}  // namespace devtools_crosstool_autofdo
