#include "llvm_propeller_chain_cluster_builder.h"

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_node_chain.h"

namespace devtools_crosstool_autofdo {

// This function builds clusters of node chains and returns them in a vector.
// After this is called, all clusters are moved to the this vector and the
// clusters_ map becomes empty.
std::vector<std::unique_ptr<ChainCluster>>
ChainClusterBuilder::BuildClusters() {
  // TODO(rahmanl) Implement MergeClusters Ref: b/154756435.
  std::vector<std::unique_ptr<ChainCluster>> built_clusters;
  for (auto &elem : clusters_)
    built_clusters.push_back(std::move(elem.second));
  clusters_.clear();
  return built_clusters;
}

}  // namespace devtools_crosstool_autofdo
