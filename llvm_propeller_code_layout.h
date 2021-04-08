#ifndef AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_
#define AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_

#if defined(HAVE_LLVM)
#include <map>
#include <unordered_map>
#include <vector>

#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {

// This struct represents the layout information for every function
struct FuncLayoutClusterInfo {
  // This struct represents a cluster of basic blocks (belong to the function
  // associated with func_symbol) which are contiguous in the layout.
  struct BBCluster {
    // Index of this cluster in the global layout (zero-based).
    const unsigned layout_index;

    // Indices of basic blocks in this cluster.
    std::vector<unsigned> bb_indexes;

    // Constructor for building a BB cluster. The 'bb_indexes' vector must be
    // populated afterwards.
    explicit BBCluster(unsigned _layout_index) : layout_index(_layout_index) {}
  };

  // Associated CFG.
  const ControlFlowGraph *cfg;

  // Clusters pertaining to this CFG.
  std::vector<BBCluster> clusters;

  // Intra-function score of this CFG in the original layout.
  uint64_t original_intra_score;

  // Intra-function score of this CFG in the computed layout.
  uint64_t optimized_intra_score;

  // Index of the function's cold cluster within the cold part.
  const unsigned cold_cluster_layout_index;

  // Basic constructor. 'clusters' vector must be populated afterwards.
  FuncLayoutClusterInfo(const ControlFlowGraph *cfg_,
                        uint64_t original_intra_score_,
                        uint64_t optimized_intra_score_,
                        unsigned cold_cluster_layout_index_)
      : cfg(cfg_),
        original_intra_score(original_intra_score_),
        optimized_intra_score(optimized_intra_score_),
        cold_cluster_layout_index(cold_cluster_layout_index_) {}
};

// CodeLayoutResult holds the result of the layout algorithm which is consumed
// by the profile writer. It contains a mapping from the function ordinal to the
// layout cluster info.
using CodeLayoutResult = std::unordered_map<uint64_t, FuncLayoutClusterInfo>;

class CodeLayout {
 public:
  explicit CodeLayout(const std::vector<ControlFlowGraph *> &cfgs)
      : cfgs_(cfgs) {}

  // This performs code layout on all hot cfgs in the prop_prof_writer instance
  // and returns the global order information for all function.
  CodeLayoutResult OrderAll();

 private:
  // CFGs targeted for code layout.
  const std::vector<ControlFlowGraph*> cfgs_;
};

}  // namespace devtools_crosstool_autofdo
#endif
#endif  // AUTOFDO_LLVM_PROPELLER_CODE_LAYOUT_H_
