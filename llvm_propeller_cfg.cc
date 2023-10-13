#include "llvm_propeller_cfg.h"

#include <algorithm>
#include <memory>
#include <numeric>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"

namespace devtools_crosstool_autofdo {
std::string CFGNode::GetName() const {
  std::string bb_name = cfg_->GetPrimaryName().str();
  if (!is_entry()) {
    bb_name += ".";
    bb_name += std::to_string(bb_index_);
  }
  return bb_name;
}

void ControlFlowGraph::CreateNodes(
    const llvm::object::BBAddrMap &func_bb_addr_map, uint64_t ordinal) {
  CHECK(nodes_.empty());
  int bb_index = 0;
  for (const auto &bb_entry : func_bb_addr_map.BBEntries) {
    nodes_.insert(std::make_unique<CFGNode>(
        /*symbol_ordinal=*/ordinal++,
        /*addr=*/func_bb_addr_map.Addr + bb_entry.Offset,
        /*bb_index=*/bb_index++,
        /*size=*/bb_entry.Size, /*is_landing_pad=*/bb_entry.isEHPad(),
        /*cfg=*/this));
    if (bb_entry.isEHPad()) ++n_landing_pads_;
  }
}

CFGEdge *ControlFlowGraph::CreateEdge(CFGNode *from, CFGNode *to,
                                      uint64_t weight,
                                      CFGEdge::Kind kind) {
  auto edge = std::make_unique<CFGEdge>(from, to, weight, kind);
  auto* ret = edge.get();
  auto has_duplicates =
      [from, to](const std::vector<std::unique_ptr<CFGEdge>> &edges) {
        for (auto &e : edges)
          if (e->src_ == from && e->sink_ == to) return true;
        return false;
      };
  (void)(has_duplicates);  // For release build warning.
  if (from->cfg() == to->cfg()) {
    CHECK(!has_duplicates(intra_edges_));
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

void ControlFlowGraph::CalculateNodeFreqs() {
  if (nodes_.empty()) return;
  ForEachNodeRef([&](CFGNode &node) {
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

    uint64_t max_call_out = 0;
    uint64_t max_ret_in = 0;

    // Total incoming edge frequency to the node's entry (first instruction).
    uint64_t sum_in = 0;
    // Total outgoing edge frequency from the node's exit (last instruction).
    uint64_t sum_out = 0;

    for (auto *out_edges : {&node.inter_outs_, &node.intra_outs_}) {
      for (auto &edge : *out_edges) {
        if (edge->IsCall())
          max_call_out = std::max(max_call_out, edge->weight());
        else
          sum_out += edge->weight();
      }
    }

    for (auto *in_edges : {&node.inter_ins_, &node.intra_ins_}) {
      for (auto &edge : *in_edges) {
        if (edge->IsReturn())
          max_ret_in = std::max(max_call_out, edge->weight());
        else
          sum_in += edge->weight();
      }
    }

    node.set_freq(std::max({max_call_out, max_ret_in, sum_out, sum_in}));

    this->hot_tag_ |= (node.freq() != 0);
    if (node.is_landing_pad() && node.freq() != 0) ++n_hot_landing_pads_;
  });
}

void ControlFlowGraph::WriteDotFormat(std::ostream &os,
    const absl::flat_hash_map<int, int> &layout_index_map) const {
  os << "digraph {\n";
  os << "label=\"" << GetPrimaryName().str() << "\"\n";
  os << "forcelabels=true;\n";
  for (const auto &node : nodes_) {
    os << node->GetDotFormatLabel() << " [xlabel=\"" << node->freq_ << "#"
       << node->size_ << "\"];\n";
  }
  for (const auto &edge : intra_edges_) {
    bool layout_edge = layout_index_map.at(edge->sink_->bb_index()) -
                           layout_index_map.at(edge->src_->bb_index()) ==
                       1;
    os << edge->src_->GetDotFormatLabel() << " -> " <<
                  edge->sink_->GetDotFormatLabel() << "[ label=\"" <<
                  edge->GetDotFormatLabel() << "\", color =\"" <<
                  (layout_edge ? "red" : "black") << "\"];\n";
  }
  os << "}\n";
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
}  // namespace devtools_crosstool_autofdo
