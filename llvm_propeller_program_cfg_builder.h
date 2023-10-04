#ifndef AUTOFDO_LLVM_PROPELLER_PROGRAM_CFG_BUILDER_H_
#define AUTOFDO_LLVM_PROPELLER_PROGRAM_CFG_BUILDER_H_

#include <memory>
#include <utility>

#include "addr2cu.h"
#include "lbr_aggregation.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_program_cfg.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "llvm/Support/MemoryBuffer.h"

namespace devtools_crosstool_autofdo {

class ProgramCfgBuilder {
 public:
  // Constructs a `ProgramCfgBuilder` initialized with CFGs in `program_cfg` (if
  // not nullptr) and which uses `binary_address_mapper` to map binary addresses
  // to basic blocks.
  // Does not take ownership of `binary_address_mapper`, which must refer to a
  // valid `BinaryAddressMapper` that outlives the constructed
  // `ProgramCfgBuilder`.
  // Moves all cfgs from `program_cfg` into this builder (if not nullptr) to
  // aggregate and apply the profile on top of them.
  ProgramCfgBuilder(const BinaryAddressMapper *binary_address_mapper,
                    PropellerStats &stats,
                    std::unique_ptr<ProgramCfg> program_cfg = nullptr)
      : binary_address_mapper_(binary_address_mapper), stats_(&stats) {
    if (program_cfg != nullptr)
      cfgs_ = std::move(*std::move(program_cfg)).release_cfgs_by_index();
  }

  ProgramCfgBuilder(const ProgramCfgBuilder &) = delete;
  ProgramCfgBuilder &operator=(const ProgramCfgBuilder &) = delete;
  ProgramCfgBuilder(ProgramCfgBuilder &&) = default;
  ProgramCfgBuilder &operator=(ProgramCfgBuilder &&) = default;

  // Creates profile CFGs using the LBR profile in `lbr_aggregation`.
  // `file_content` points to ELF content. And `addr2cu`, if provided, will be
  // used to retrieve module names for CFGs. This function does not assume
  // ownership of it.
  absl::StatusOr<std::unique_ptr<ProgramCfg>> Build(
      const LbrAggregation &lbr_aggregation,
      std::unique_ptr<llvm::MemoryBuffer> file_content,
      Addr2Cu *addr2cu = nullptr) &&;

 private:
  // Creates and returns an edge from `from_bb` to `to_bb` (specified by their
  // BbHandle index) with the given `weight` and `edge_kind` and associates it
  // to the corresponding nodes specified by `tmp_node_map`. Finally inserts the
  // edge into `tmp_edge_map` with the key being the pair `{from_bb, to_bb}`.
  CFGEdge *InternalCreateEdge(
      int from_bb_index, int to_bb_index, int weight, CFGEdge::Kind edge_kind,
      const absl::flat_hash_map<CFGNode::InterCfgId, CFGNode *> &tmp_node_map,
      absl::flat_hash_map<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>,
                          CFGEdge *> *tmp_edge_map);

  void CreateFallthroughs(
      const LbrAggregation &lbr_aggregation,
      const absl::flat_hash_map<CFGNode::InterCfgId, CFGNode *> &tmp_node_map,
      absl::flat_hash_map<std::pair<int, int>, int>
          *tmp_bb_fallthrough_counters,
      absl::flat_hash_map<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>,
                          CFGEdge *> *tmp_edge_map);

  // Create control flow graph edges from branch_counters_. For each address
  // pair
  // <from_addr, to_addr> in "branch_counters_", we translate it to
  // <from_symbol, to_symbol> and by using tmp_node_map, we further translate
  // it to <from_node, to_node>, and finally create a CFGEdge for such CFGNode
  // pair.
  absl::Status CreateEdges(
      const LbrAggregation &lbr_aggregation,
      const absl::flat_hash_map<CFGNode::InterCfgId, CFGNode *> &node_map);

  const BinaryAddressMapper *binary_address_mapper_;
  PropellerStats *stats_;
  // Maps from function index to its CFG.
  absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_PROGRAM_CFG_BUILDER_H_
