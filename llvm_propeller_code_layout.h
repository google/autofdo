#ifndef AUTOFDOLLVM_PROPELLER_CODE_LAYOUT_H_
#define AUTOFDOLLVM_PROPELLER_CODE_LAYOUT_H_

#include <memory>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_cluster_builder.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_function_cluster_info.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_program_cfg.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/types/span.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {

// Runs `CodeLayout` on every section in `program_cfg` and returns
// the code layout results as a map keyed by section names, and valued by the
// `FunctionClusterInfo` of all functions in each section.
absl::btree_map<llvm::StringRef, std::vector<FunctionClusterInfo>>
GenerateLayoutBySection(const ProgramCfg &program_cfg,
                        const PropellerCodeLayoutParameters &code_layout_params,
                        PropellerStats::CodeLayoutStats &code_layout_stats);

class CodeLayout {
 public:
  // `initial_chains` describes the cfg nodes that must be placed in single
  // chains before chain merging. It is specified as a map from every function
  // index to the vector of initial node chains for the corresponding CFG. Each
  // node chain is specified by a vector of bb_indexes of its nodes.
  CodeLayout(
      const PropellerCodeLayoutParameters &code_layout_params,
      const std::vector<const ControlFlowGraph *> &cfgs,
      absl::flat_hash_map<int, std::vector<std::vector<CFGNode::IntraCfgId>>>
          initial_chains = {})
      : code_layout_scorer_(code_layout_params),
        cfgs_(cfgs),
        initial_chains_(std::move(initial_chains)) {}

  // This performs code layout on all hot cfgs in the prop_prof_writer instance
  // and returns the global order information for all function.
  std::vector<FunctionClusterInfo> OrderAll();

  PropellerStats::CodeLayoutStats stats() const { return stats_; }

 private:
  const PropellerCodeLayoutScorer code_layout_scorer_;
  // CFGs targeted for code layout.
  const std::vector<const ControlFlowGraph *> cfgs_;
  // Initial node chains, specified as a map from every function index to the
  // vector of initial node chains for the corresponding CFG. Each node chain is
  // specified by a vector of bb_indexes of its nodes.
  const absl::flat_hash_map<int, std::vector<std::vector<CFGNode::IntraCfgId>>>
      initial_chains_;
  PropellerStats::CodeLayoutStats stats_;

  // Returns the intra-procedural ext-tsp scores for the given CFGs given a
  // function for getting the address of each CFG node.
  // This is called by ComputeOrigLayoutScores and ComputeOptLayoutScores below.
  absl::flat_hash_map<int, CFGScore> ComputeCfgScores(
      absl::FunctionRef<uint64_t(const CFGNode *)>);

  // Returns the intra-procedural ext-tsp scores for the given CFGs under the
  // original layout.
  absl::flat_hash_map<int, CFGScore> ComputeOrigLayoutScores();

  // Returns the intra-procedural ext-tsp scores for the given CFGs under the
  // new layout, which is described by the 'clusters' parameter.
  absl::flat_hash_map<int, CFGScore> ComputeOptLayoutScores(
      absl::Span<const std::unique_ptr<const ChainCluster>> clusters);
};

}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDOLLVM_PROPELLER_CODE_LAYOUT_H_
