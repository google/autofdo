#include "llvm_propeller_program_cfg_builder.h"

#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "addr2cu.h"
#include "bb_handle.h"
#include "lbr_aggregation.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_program_cfg.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/log/log.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/MemoryBuffer.h"

namespace devtools_crosstool_autofdo {

namespace {
// Creates and returns a vector CFGNodes containing a `CFGNode` for every
// BBAddrMap entry in `func_bb_addr_map` in the same order.
std::vector<std::unique_ptr<CFGNode>> CreateCfgNodes(
    int function_index, const llvm::object::BBAddrMap &func_bb_addr_map) {
  std::vector<std::unique_ptr<CFGNode>> nodes;
  for (int bb_index = 0; bb_index < func_bb_addr_map.BBEntries.size();
       ++bb_index) {
    const auto &bb_entry = func_bb_addr_map.BBEntries[bb_index];
    nodes.push_back(std::make_unique<CFGNode>(
        func_bb_addr_map.Addr + bb_entry.Offset, bb_index, bb_entry.ID,
        bb_entry.Size, bb_entry.MD, function_index));
  }
  return nodes;
}
}  // namespace

absl::StatusOr<std::unique_ptr<ProgramCfg>> ProgramCfgBuilder::Build(
    const LbrAggregation &lbr_aggregation,
    std::unique_ptr<llvm::MemoryBuffer> file_content, Addr2Cu *addr2cu) && {
  // Temporary map from Id -> CFGNode.
  absl::flat_hash_map<CFGNode::InterCfgId, CFGNode *> node_map;
  // Insert node mappings for initial CFGs.
  for (auto &[cfg_index, cfg] : cfgs_) {
    for (const std::unique_ptr<CFGNode> &node : cfg->nodes()) {
      node_map.insert({node->inter_cfg_id(), node.get()});
    }
  }
  for (int func_index : binary_address_mapper_->selected_functions()) {
    const llvm::object::BBAddrMap &func_bb_addr_map =
        binary_address_mapper_->bb_addr_map()[func_index];

    std::optional<llvm::StringRef> module_name = std::nullopt;
    if (addr2cu) {
      absl::StatusOr<absl::string_view> res =
          addr2cu->GetCompileUnitFileNameForCodeAddress(func_bb_addr_map.Addr);
      if (res.ok()) module_name = llvm::StringRef(res->data(), res->size());
    }

    CHECK(!func_bb_addr_map.BBEntries.empty());
    // Skip functions which already have a CFG created for them.
    if (cfgs_.contains(func_index)) continue;

    const BinaryAddressMapper::FunctionSymbolInfo symbol_info =
        binary_address_mapper_->symbol_info_map().at(func_index);

    auto cfg = std::make_unique<ControlFlowGraph>(
        symbol_info.section_name, func_index, module_name, symbol_info.aliases,
        CreateCfgNodes(func_index, func_bb_addr_map));
    // Setup mapping from Ids to nodes.
    for (const std::unique_ptr<CFGNode> &node : cfg->nodes())
      node_map.insert({node->inter_cfg_id(), node.get()});
    CHECK_EQ(cfg->nodes().size(), func_bb_addr_map.BBEntries.size());
    stats_->nodes_created += cfg->nodes().size();
    cfgs_.insert({func_index, std::move(cfg)});
    ++stats_->cfgs_created;
  }
  if (absl::Status status = CreateEdges(lbr_aggregation, node_map);
      !status.ok()) {
    return absl::InternalError(absl::StrCat(
        "Unable to create edges from LBR profile: ", status.message()));
  }

  absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs;
  for (auto &[cfg_index, cfg] : cfgs_) {
    ControlFlowGraph::NodeFrequencyStats cfg_stats =
        cfg->GetNodeFrequencyStats();
    if (cfg_stats.n_hot_blocks == 0) continue;
    stats_->hot_basic_blocks += cfg_stats.n_hot_blocks;
    stats_->hot_empty_basic_blocks += cfg_stats.n_hot_empty_blocks;
    if (cfg_stats.n_hot_landing_pads != 0) ++stats_->cfgs_with_hot_landing_pads;
    cfgs.emplace(cfg_index, std::move(cfg));
  }
  cfgs_.clear();
  return std::make_unique<ProgramCfg>(std::move(cfgs), std::move(file_content));
}

// Create edges for fallthroughs.
// 1. translate fallthrough pairs "<from, to>"s -> "<from_sym, to_sym>"s.
// 2. calculate all symbols between "from_sym" and "to_sym", so we get the
//    symbol path: <from_sym, internal_sym1, internal_sym2, ... , internal_symn,
//    to_sym>.
// 3. create edges and apply weights for the above path.
void ProgramCfgBuilder::CreateFallthroughs(
    const LbrAggregation &lbr_aggregation,
    const absl::flat_hash_map<CFGNode::InterCfgId, CFGNode *> &tmp_node_map,
    absl::flat_hash_map<std::pair<int, int>, int> *tmp_bb_fallthrough_counters,
    absl::flat_hash_map<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>,
                        CFGEdge *> *tmp_edge_map) {
  for (auto [fallthrough, cnt] : lbr_aggregation.fallthrough_counters) {
    // A fallthrough from A to B implies a branch to A followed by a branch
    // from B. Therefore we respectively use BranchDirection::kTo and
    // BranchDirection::kFrom for A and B when calling
    // `FindBbHandleIndexUsingBinaryAddress` to find their associated blocks.
    std::optional<int> from_index =
        binary_address_mapper_->FindBbHandleIndexUsingBinaryAddress(
            fallthrough.from, BranchDirection::kTo);
    std::optional<int> to_index =
        binary_address_mapper_->FindBbHandleIndexUsingBinaryAddress(
            fallthrough.to, BranchDirection::kFrom);
    if (from_index && to_index)
      (*tmp_bb_fallthrough_counters)[{*from_index, *to_index}] += cnt;
  }

  for (auto &i : *tmp_bb_fallthrough_counters) {
    int fallthrough_from = i.first.first, fallthrough_to = i.first.second;
    int weight = i.second;
    if (fallthrough_from == fallthrough_to ||
        !binary_address_mapper_->CanFallThrough(fallthrough_from,
                                                fallthrough_to))
      continue;

    for (int sym = fallthrough_from; sym <= fallthrough_to - 1; ++sym) {
      auto *fallthrough_edge = InternalCreateEdge(
          sym, sym + 1, weight, CFGEdge::Kind::kBranchOrFallthough,
          tmp_node_map, tmp_edge_map);
      if (!fallthrough_edge) break;
    }
  }
}

CFGEdge *ProgramCfgBuilder::InternalCreateEdge(
    int from_bb_index, int to_bb_index, int weight, CFGEdge::Kind edge_kind,
    const absl::flat_hash_map<CFGNode::InterCfgId, CFGNode *> &tmp_node_map,
    absl::flat_hash_map<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>,
                        CFGEdge *> *tmp_edge_map) {
  BbHandle from_bb = binary_address_mapper_->bb_handles().at(from_bb_index);
  BbHandle to_bb = binary_address_mapper_->bb_handles().at(to_bb_index);
  // Compute the IDs of the corresponding basic blocks.
  CFGNode::InterCfgId from_bb_id = {from_bb.function_index,
                                    {from_bb.bb_index, 0}};
  CFGNode::InterCfgId to_bb_id = {to_bb.function_index, {to_bb.bb_index, 0}};
  CFGEdge *edge = nullptr;
  auto i = tmp_edge_map->find(std::make_pair(from_bb_id, to_bb_id));
  if (i != tmp_edge_map->end()) {
    edge = i->second;
    if (edge->kind() != edge_kind) {
      LOG(WARNING) << "Edges with same src and sink have different type: "
                   << CFGEdgeNameFormatter(edge) << " has type "
                   << CFGEdge::GetCfgEdgeKindString(edge_kind) << " and "
                   << CFGEdge::GetCfgEdgeKindString(edge->kind());
      ++stats_->edges_with_same_src_sink_but_different_type;
    }
    edge->IncrementWeight(weight);
  } else {
    auto from_ni = tmp_node_map.find(from_bb_id),
         to_ni = tmp_node_map.find(to_bb_id);
    if (from_ni == tmp_node_map.end() || to_ni == tmp_node_map.end())
      return nullptr;
    CFGNode *from_node = from_ni->second;
    CFGNode *to_node = to_ni->second;
    DCHECK(from_node && to_node);
    ControlFlowGraph &from_cfg = *cfgs_.at(from_bb.function_index);
    ControlFlowGraph &to_cfg = *cfgs_.at(to_bb.function_index);
    edge = cfgs_.at(from_bb.function_index)
               ->CreateEdge(from_node, to_node, weight, edge_kind,
                            (from_cfg.section_name() != to_cfg.section_name()));
    ++stats_->edges_created_by_kind[edge_kind];
    tmp_edge_map->emplace(std::piecewise_construct,
                          std::forward_as_tuple(from_bb_id, to_bb_id),
                          std::forward_as_tuple(edge));
  }
  stats_->total_edge_weight_by_kind[edge_kind] += weight;
  return edge;
}

// Create control flow graph edges from branch_counters_. For each address pair
// <from_addr, to_addr> in "branch_counters_", we translate it to <from_symbol,
// to_symbol> and by using tmp_node_map, we further translate it to <from_node,
// to_node>, and finally create a CFGEdge for such CFGNode pair.
absl::Status ProgramCfgBuilder::CreateEdges(
    const LbrAggregation &lbr_aggregation,
    const absl::flat_hash_map<CFGNode::InterCfgId, CFGNode *> &tmp_node_map) {
  // Temp map that records which CFGEdges are created, so we do not re-create
  // edges. Note this is necessary: although
  // "branch_counters_" have no duplicated <from_addr, to_addr> pairs, the
  // translated <from_bb, to_bb> may have duplicates.
  absl::flat_hash_map<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>,
                      CFGEdge *>
      tmp_edge_map;

  absl::flat_hash_map<std::pair<int, int>, int> tmp_bb_fallthrough_counters;

  int weight_on_dubious_edges = 0;
  int edges_recorded = 0;
  for (auto [branch, weight] : lbr_aggregation.branch_counters) {
    ++edges_recorded;
    std::optional<int> from_bb_index =
        binary_address_mapper_->FindBbHandleIndexUsingBinaryAddress(
            branch.from, BranchDirection::kFrom);
    std::optional<int> to_bb_index =
        binary_address_mapper_->FindBbHandleIndexUsingBinaryAddress(
            branch.to, BranchDirection::kTo);
    if (!to_bb_index.has_value()) continue;

    BbHandle to_bb_handle = binary_address_mapper_->bb_handles()[*to_bb_index];
    BbHandle from_bb_handle =
        from_bb_index.has_value()
            ? binary_address_mapper_->bb_handles()[*from_bb_index]
            : BbHandle{.function_index = -1, .bb_index = -1};
    // This is to handle the case when a call is the last instr of a basicblock,
    // and a return to the beginning of the next basic block, change the to_sym
    // to the basic block just before and add fallthrough between the two
    // symbols. After this code executes, "to" can never be the beginning
    // of a basic block for returns.
    // We also account for returns from external library functions which happen
    // when from_sym is null.
    if ((!from_bb_index.has_value() ||
         binary_address_mapper_->GetBBEntry(from_bb_handle).hasReturn() ||
         to_bb_handle.function_index != from_bb_handle.function_index) &&
        binary_address_mapper_->GetFunctionEntry(to_bb_handle).Addr !=
            branch.to &&  // Not a call
        // Jump to the beginning of the basicblock
        branch.to == binary_address_mapper_->GetAddress(to_bb_handle)) {
      if (to_bb_handle.bb_index != 0) {
        // Account for the fall-through between callSiteSym and toSym.
        tmp_bb_fallthrough_counters[{*to_bb_index - 1, *to_bb_index}] += weight;
        // Reassign to_bb to be the actual callsite symbol entry.
        --*to_bb_index;
        to_bb_handle = binary_address_mapper_->bb_handles()[*to_bb_index];
      } else {
        LOG(WARNING) << "*** Warning: Could not find the right "
                        "call site sym for : "
                     << binary_address_mapper_->GetName(to_bb_handle);
      }
    }
    if (!from_bb_index.has_value()) continue;
    if (!binary_address_mapper_->GetBBEntry(from_bb_handle).hasReturn() &&
        binary_address_mapper_->GetAddress(to_bb_handle) != branch.to) {
      // Jump is not a return and its target is not the beginning of a function
      // or a basic block.
      weight_on_dubious_edges += weight;
    }

    CFGEdge::Kind edge_kind = CFGEdge::Kind::kBranchOrFallthough;
    if (binary_address_mapper_->GetFunctionEntry(to_bb_handle).Addr ==
        branch.to) {
      edge_kind = CFGEdge::Kind::kCall;
    } else if (branch.to != binary_address_mapper_->GetAddress(to_bb_handle) ||
               binary_address_mapper_->GetBBEntry(from_bb_handle).hasReturn()) {
      edge_kind = CFGEdge::Kind::kRet;
    }
    InternalCreateEdge(from_bb_index.value(), to_bb_index.value(), weight,
                       edge_kind, tmp_node_map, &tmp_edge_map);
  }

  if (weight_on_dubious_edges /
          static_cast<double>(stats_->total_edge_weight_created()) >
      0.3) {
    return absl::InternalError(absl::StrFormat(
        "Too many jumps into middle of basic blocks detected, "
        "probably because of source drift (%d out of %d).",
        weight_on_dubious_edges, stats_->total_edge_weight_created()));
  }

  if (stats_->total_edges_created() / static_cast<double>(edges_recorded) <
      0.0005) {
    return absl::InternalError(
        "Fewer than 0.05% recorded jumps are converted into CFG edges, "
        "probably because of source drift.");
  }

  CreateFallthroughs(lbr_aggregation, tmp_node_map,
                     &tmp_bb_fallthrough_counters, &tmp_edge_map);
  return absl::OkStatus();
}
}  // namespace devtools_crosstool_autofdo
