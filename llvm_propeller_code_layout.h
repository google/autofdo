#ifndef AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_
#define AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_

#if defined(HAVE_LLVM)
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_cluster_builder.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "llvm_propeller_node_chain_assembly.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_join.h"

namespace devtools_crosstool_autofdo {

struct CodeLayoutStats {
  // Number of assemblies applied by NodeChainBuilder separated by merge_order.
  // These are the assemblies with which NodeChainBuilder::MergeChains(assembly)
  // has been called.
  absl::flat_hash_map<MergeOrder, int> n_assemblies_by_merge_order;
  // Number of initial single-node chains.
  int n_single_node_chains = 0;
  // Number of initial multi-node chains.
  int n_multi_node_chains = 0;

  std::string DebugString() const {
    std::string result;
    absl::StrAppend(
        &result, "Merge order stats: ",
        absl::StrJoin(
            n_assemblies_by_merge_order, ", ",
            [](std::string *out, const std::pair<MergeOrder, int> &entry) {
              absl::StrAppend(out, "[", GetMergeOrderName(entry.first), ":",
                              entry.second, "]");
            }),
        "\n");
    absl::StrAppend(&result, "Initial chains stats: single-node chains: [",
                    n_single_node_chains, "] multi-node chains: [",
                    n_multi_node_chains, "]");
    return result;
  }
};

struct CFGScore {
  // Total score across all intra-function edges in a CFG.
  uint64_t intra_score = 0;
  // Total score across all inter-function edges for a CFG. We consider
  // only the outgoing edges to prevent from double counting.
  uint64_t inter_out_score = 0;
};

using CFGScoreMapTy = absl::flat_hash_map<const ControlFlowGraph *, CFGScore>;

// This struct represents the layout information for one function, that is every
// basic block cluster and its layout index within the global ordering.
struct FunctionClusterInfo {
  // This struct represents a cluster of basic blocks (belong to the function
  // associated with func_symbol) which are contiguous in the layout.
  struct BBCluster {
    // Index of this cluster in the global layout (zero-based).
    unsigned layout_index;

    // Indices of basic blocks in this cluster.
    std::vector<int> bb_indexes;

    // Constructor for building a BB cluster. The 'bb_indexes' vector must be
    // populated afterwards.
    explicit BBCluster(unsigned _layout_index) : layout_index(_layout_index) {}
  };

  // Associated CFG.
  ControlFlowGraph *cfg = nullptr;

  // Clusters pertaining to this CFG.
  std::vector<BBCluster> clusters = {};

  // Score of this CFG in the original layout.
  CFGScore original_score = {};

  // Score of this CFG in the computed layout.
  CFGScore optimized_score = {};

  // Index of the function's cold cluster within the cold part.
  unsigned cold_cluster_layout_index = 0;
};

class CodeLayout {
 public:
  explicit CodeLayout(const PropellerCodeLayoutParameters &code_layout_params,
                      const std::vector<ControlFlowGraph *> &cfgs)
      : code_layout_scorer_(code_layout_params), cfgs_(cfgs) {}

  // This performs code layout on all hot cfgs in the prop_prof_writer instance
  // and returns the global order information for all function.
  std::vector<FunctionClusterInfo> OrderAll();

 private:
  const PropellerCodeLayoutScorer code_layout_scorer_;
  // CFGs targeted for code layout.
  const std::vector<ControlFlowGraph *> cfgs_;
  CodeLayoutStats stats_;

  // Returns the intra-procedural ext-tsp scores for the given CFGs given a
  // function for getting the address of each CFG node.
  // This is called by ComputeOrigLayoutScores and ComputeOptLayoutScores below.
  CFGScoreMapTy ComputeCfgScores(absl::FunctionRef<uint64_t(const CFGNode *)>);

  // Returns the intra-procedural ext-tsp scores for the given CFGs under the
  // original layout.
  CFGScoreMapTy ComputeOrigLayoutScores();

  // Returns the intra-procedural ext-tsp scores for the given CFGs under the
  // new layout, which is described by the 'clusters' parameter.
  CFGScoreMapTy ComputeOptLayoutScores(
      const std::vector<std::unique_ptr<const ChainCluster>> &clusters);
};

}  // namespace devtools_crosstool_autofdo
#endif
#endif  // AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_
