#ifndef AUTOFDOLLVM_PROPELLER_FUNCTION_CLUSTER_INFO_H_
#define AUTOFDOLLVM_PROPELLER_FUNCTION_CLUSTER_INFO_H_

#include <vector>

#include "llvm_propeller_cfg.h"

namespace devtools_crosstool_autofdo {

struct CFGScore {
  // Total score across all intra-function edges in a CFG.
  double intra_score = 0;
  // Total score across all inter-function edges for a CFG. We consider
  // only the outgoing edges to prevent from double counting.
  double inter_out_score = 0;
};

// This struct represents the layout information for one function, that is every
// basic block cluster and its layout index within the global ordering.
struct FunctionClusterInfo {
  // This struct represents a cluster of basic blocks (belong to the function
  // associated with func_symbol) which are contiguous in the layout.
  struct BBCluster {
    // Index of this cluster in the global layout (zero-based).
    unsigned layout_index;

    // Ids of basic blocks in this cluster.
    std::vector<CFGNode::FullIntraCfgId> full_bb_ids;

    // Constructor for building a BB cluster. The 'full_bb_ids' vector must be
    // populated afterwards.
    explicit BBCluster(unsigned _layout_index) : layout_index(_layout_index) {}
  };

  // Associated CFG's function_index.
  int function_index = -1;

  // Clusters pertaining to this CFG.
  std::vector<BBCluster> clusters = {};

  // Score of this CFG in the original layout.
  CFGScore original_score = {};

  // Score of this CFG in the computed layout.
  CFGScore optimized_score = {};

  // Index of the function's cold cluster within the cold part.
  unsigned cold_cluster_layout_index = 0;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDOLLVM_PROPELLER_FUNCTION_CLUSTER_INFO_H_
