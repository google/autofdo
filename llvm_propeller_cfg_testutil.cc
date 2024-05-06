#include "llvm_propeller_cfg_testutil.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "base/logging.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/types/span.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {

std::vector<std::unique_ptr<CFGNode>> TestCfgBuilder::CreateNodesForCfg(
    int function_index, absl::Span<const NodeArg> args) {
  std::vector<std::unique_ptr<CFGNode>> nodes;
  for (const auto &node_arg : args) {
    auto node = std::make_unique<CFGNode>(
        /*addr=*/node_arg.addr,
        // Use the same bb_index and bb_id for tests.
        /*bb_index=*/node_arg.bb_index,
        /*bb_id=*/node_arg.bb_index,
        /*size=*/node_arg.size, /*metadata=*/node_arg.metadata,
        /*function_index=*/function_index);
    CHECK(nodes_by_function_and_bb_index_[node->function_index()]
              .emplace(node->bb_index(), node.get())
              .second)
        << "Duplicate bb index: " << node->bb_index();
    nodes.push_back(std::move(node));
  }
  return nodes;
}

void TestCfgBuilder::CreateIntraEdgesForCfg(
    ControlFlowGraph &cfg, absl::Span<const IntraEdgeArg> args) {
  for (const auto &edge_arg : args) {
    cfg.CreateEdge(nodes_by_function_and_bb_index_.at(cfg.function_index())
                       .at(edge_arg.from_bb_index),
                   nodes_by_function_and_bb_index_.at(cfg.function_index())
                       .at(edge_arg.to_bb_index),
                   edge_arg.weight, edge_arg.kind, /*inter_section=*/false);
  }
}

void TestCfgBuilder::CreateInterEdges(absl::Span<const InterEdgeArg> args) {
  for (const auto &edge_arg : args) {
    std::unique_ptr<ControlFlowGraph> &from_cfg =
        cfgs_by_function_index_.at(edge_arg.from_function_index);
    std::unique_ptr<ControlFlowGraph> &to_cfg =
        cfgs_by_function_index_.at(edge_arg.to_function_index);
    CFGNode *from_node =
        nodes_by_function_and_bb_index_.at(edge_arg.from_function_index)
            .at(edge_arg.from_bb_index);
    CFGNode *to_node =
        nodes_by_function_and_bb_index_.at(edge_arg.to_function_index)
            .at(edge_arg.to_bb_index);
    from_cfg->CreateEdge(from_node, to_node, edge_arg.weight, edge_arg.kind,
                         from_cfg->section_name() != to_cfg->section_name());
  }
}

absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>>
TestCfgBuilder::Build() && {
  for (const auto &cfg_arg : multi_cfg_arg_.cfg_args) {
    auto cfg = std::make_unique<ControlFlowGraph>(
        cfg_arg.section_name, cfg_arg.function_index, std::nullopt,
        llvm::SmallVector<llvm::StringRef, 1>({cfg_arg.function_name}),
        CreateNodesForCfg(cfg_arg.function_index, cfg_arg.node_args));
    CreateIntraEdgesForCfg(*cfg, cfg_arg.edge_args);
    CHECK(
        cfgs_by_function_index_.emplace(cfg_arg.function_index, std::move(cfg))
            .second)
        << "Duplicate function index: " << cfg_arg.function_index;
  }
  CreateInterEdges(multi_cfg_arg_.inter_edge_args);
  return std::move(cfgs_by_function_index_);
}
}  // namespace devtools_crosstool_autofdo
