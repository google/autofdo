#ifndef AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_
#define AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_

#if defined(HAVE_LLVM)
#include <unordered_map>
#include <vector>

#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_cluster_builder.h"
#include "llvm_propeller_code_layout_scorer.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {

struct CFGScore {
  // Total score across all intra-function edges in a CFG.
  uint64_t intra_score = 0;
  // Total score across all inter-function edges for a CFG. We consider
  // only the outgoing edges to prevent from double counting.
  uint64_t inter_out_score = 0;
};

using CFGScoreMapTy = absl::flat_hash_map<const ControlFlowGraph *, CFGScore>;

// This struct represents the layout information for every function
struct FuncLayoutClusterInfo {
  // This struct represents a cluster of basic blocks (belong to the function
  // associated with func_symbol) which are contiguous in the layout.
  struct BBCluster {
    // Index of this cluster in the global layout (zero-based).
    unsigned layout_index;

    // Indices of basic blocks in this cluster.
    std::vector<unsigned> bb_indexes;

    // Constructor for building a BB cluster. The 'bb_indexes' vector must be
    // populated afterwards.
    explicit BBCluster(unsigned _layout_index) : layout_index(_layout_index) {}
  };

  // Associated CFG.
  const ControlFlowGraph *cfg = nullptr;

  // Clusters pertaining to this CFG.
  std::vector<BBCluster> clusters = {};

  // Score of this CFG in the original layout.
  const CFGScore original_score;

  // Score of this CFG in the computed layout.
  const CFGScore optimized_score;

  // Index of the function's cold cluster within the cold part.
  const unsigned cold_cluster_layout_index = 0;
};

// CodeLayoutResult holds the result of the layout algorithm which is consumed
// by the profile writer. It contains a mapping from the function ordinal to the
// layout cluster info.
using CodeLayoutResult = std::unordered_map<uint64_t, FuncLayoutClusterInfo>;

class CodeLayout {
 public:
  explicit CodeLayout(const PropellerCodeLayoutParameters &code_layout_params,
                      const std::vector<ControlFlowGraph *> &cfgs)
      : code_layout_scorer_(code_layout_params), cfgs_(cfgs) {}

  // This performs code layout on all hot cfgs in the prop_prof_writer instance
  // and returns the global order information for all function.
  CodeLayoutResult OrderAll();

 private:
  const PropellerCodeLayoutScorer code_layout_scorer_;
  // CFGs targeted for code layout.
  const std::vector<ControlFlowGraph *> cfgs_;

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
      std::vector<std::unique_ptr<ChainCluster>> &clusters);
};

}  // namespace devtools_crosstool_autofdo
#endif
#endif  // AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_
