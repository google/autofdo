#include "llvm_propeller_cfg.h"

#include <numeric>

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

CFGEdge *ControlFlowGraph::CreateEdge(CFGNode *from, CFGNode *to,
                                      uint64_t weight,
                                      typename CFGEdge::Info inf) {
  CFGEdge *edge = new CFGEdge(from, to, weight, inf);
  auto has_duplicates =
      [from, to](const std::vector<std::unique_ptr<CFGEdge>> &edges) {
        for (auto &e : edges)
          if (e->src_ == from && e->sink_ == to) return true;
        return false;
      };
  (void)(has_duplicates);  // For release build warning.
  if (from->cfg_ == to->cfg_) {
    CHECK(!has_duplicates(intra_edges_));
    from->intra_outs_.push_back(edge);
    to->intra_ins_.push_back(edge);
    intra_edges_.emplace_back(edge);
  } else {
    assert(!has_duplicates(inter_edges_));
    from->inter_outs_.push_back(edge);
    to->inter_ins_.push_back(edge);
    inter_edges_.emplace_back(edge);
  }
  return edge;
}

void ControlFlowGraph::FinishCreatingControlFlowGraph() {
  CalculateNodeFreqs();
  CoalesceColdNodes();
}

void ControlFlowGraph::ResetEntryNodeSize() {
  uint64_t non_entry_size = 0;
  ForEachNodeRef([&non_entry_size](const CFGNode &node) {
    if (!node.is_entry()) non_entry_size += node.size_;
  });
  CHECK(GetEntryNode()->size_ >= non_entry_size);
  GetEntryNode()->size_ -= non_entry_size;
}

void ControlFlowGraph::CalculateNodeFreqs() {
  if (nodes_.empty()) return;
  auto sum_edge_weight = [](const std::vector<CFGEdge *> &edges) -> uint64_t {
    return std::accumulate(
        edges.begin(), edges.end(), 0,
        [](uint64_t sum, const CFGEdge *edge) { return sum + edge->weight_; });
  };
  ForEachNodeRef([this, &sum_edge_weight](CFGNode &node) {
    uint64_t max_call_out =
        node.inter_outs_.empty()
            ? 0
            : (*std::max_element(node.inter_outs_.begin(),
                                 node.inter_outs_.end(),
                                 [](const CFGEdge *e1, const CFGEdge *e2) {
                                   return e1->weight_ < e2->weight_;
                                 }))
                  ->weight_;

    // Here is the case we must specially handle in summation of all inter_in_
    // weights.
    //
    // void foo {
    //   goo();
    // }
    //
    // int main() {
    //   for (int i = 0; i < 100; i++)
    //     foo();
    //   return 0;
    // }
    //
    // foo is called 100 times. goo is called 100 times. There is an inter call
    // edge with weight being 100 from 'main' to 'foo'. There is a return edge
    // with weight being 100 from 'goo' to 'foo'. In
    // ControlFlowGraph::CreateEdge, the two edges (one is call and one is
    // return) are both counted as inter call inward edges for 'foo', so the
    // node freq is 200, which is incorrect.
    // The following is to address b/155488527
    uint64_t sum_call_in = std::accumulate(
        node.inter_ins_.begin(), node.inter_ins_.end(), 0,
        [&node](uint64_t sum, const CFGEdge *edge) {
          CHECK(edge->sink_ == &node);
          return sum + (edge->info_ == CFGEdge::RET ? 0 : edge->weight_);
        });
    node.freq_ =
        std::max({sum_edge_weight(node.intra_outs_),
                  sum_edge_weight(node.intra_ins_), sum_call_in, max_call_out});

    this->hot_tag_ |= (node.freq_ != 0);
  });
  // Make sure entry node has a non-zero frequency if function is hot.
  if (this->hot_tag_ && GetEntryNode()->freq_ == 0)
    GetEntryNode()->freq_ = 1;
}

void ControlFlowGraph::CoalesceColdNodes() {
  for (auto i = nodes_.begin(), j = nodes_.end(); i != j;) {
    auto &n = *i;
    if (n->freq_) {
      ++i;
      continue;
    }
    // Since all edges are created from profiles, that means, all edges must
    // have weight > 0, which also means, if a node is cold (feq_ == 0), then
    // the node must not have any edges. (except incoming return edges, that is:
    // n->inter_ins_ could be non-empty)
    CHECK(n->intra_ins_.empty() && n->intra_outs_.empty() &&
          n->inter_outs_.empty());
    if (coalesced_cold_node_ != nullptr) {
      coalesced_cold_node_->size_ += n->size_;
      // As noted above, n->inter_ins_ could be non-empty, change edge sink to
      // the representative node.
      // TODO(shenhna, rahmanl): revisit "CalculateNodeFreqs" and possibly do
      // not create return edges at all, so we don't need change the edge's sink
      // here.
      for (auto &e : n->inter_ins_) e->sink_ = coalesced_cold_node_;
      i = nodes_.erase(i);
    } else {
      coalesced_cold_node_ = n.get();
      ++i;
    }
  }
}
}  // namespace devtools_crosstool_autofdo
