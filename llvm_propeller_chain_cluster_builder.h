#ifndef AUTOFDO_LLVM_PROPELLER_CHAIN_CLUSTER_BUILDER_H_
#define AUTOFDO_LLVM_PROPELLER_CHAIN_CLUSTER_BUILDER_H_

#include <unordered_map>
#include <vector>

#include "llvm_propeller_node_chain.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/container/node_hash_map.h"

namespace devtools_crosstool_autofdo {

struct ChainCluster {
  // The chains in this cluster in the merged order.
  std::vector<std::unique_ptr<NodeChain>> chains;

  // The representative chain for this cluster.
  NodeChain *delegate_chain;

  // Total size of the cluster.
  uint64_t size;

  // Total frequency of the cluster.
  uint64_t freq;

  // Gives a unique identifier for this cluster.
  uint64_t id() const { return delegate_chain->id(); }

  // Gives the execution density for this cluster.
  double exec_density() const {
    return (static_cast<double>(freq)) /
           std::max(size, static_cast<uint64_t>(1));
  }

  // Constructor for building a cluster from a single chain.
  explicit ChainCluster(std::unique_ptr<NodeChain> chain)
      : delegate_chain(chain.get()), size(chain->size_), freq(chain->freq_) {
    chains.push_back(std::move(chain));
  }

  // Helper method for iterating over all nodes in this cluster (in order).
  // Its Argument v is a function with CFGNode* as its single argument.
  template <class Visitor>
  void VisitEachNodeRef(Visitor v) {
    for (auto &chain : chains) chain->VisitEachNodeRef(v);
  }
};

class ChainClusterBuilder {
 public:
  // ChainClusterBuilder constructor: This initializes one cluster per each
  // chain and transfers the ownership of the NodeChain pointer to their
  // associated clusters.
  explicit ChainClusterBuilder(
      std::vector<std::unique_ptr<NodeChain>> &&chains) {
    for (auto &c_ptr : chains) {
      // Transfer the ownership of chains to clusters
      ChainCluster *cluster = new ChainCluster(std::move(c_ptr));
      chain_to_cluster_map_[cluster->delegate_chain] = cluster;
      auto inserted = clusters_.emplace(cluster->id(), cluster).second;
      CHECK(inserted) << "Duplicate cluster id: " << cluster->id() << ".";
    }
    chains.clear();
  }

  std::vector<std::unique_ptr<ChainCluster>> BuildClusters();

 private:
  // All clusters currently in process.
  absl::flat_hash_map<uint64_t, std::unique_ptr<ChainCluster>> clusters_;

  // This maps every chain to its containing cluster.
  // TODO(b/160191690): Remove node_hash_map before create_llvm_prof upstream
  // release to remove the dependency on absl.
  absl::flat_hash_map<NodeChain *, ChainCluster *> chain_to_cluster_map_;
};

}  // namespace devtools_crosstool_autofdo

#endif  //  AUTOFDO_LLVM_PROPELLER_CHAIN_CLUSTER_BUILDER_H_
