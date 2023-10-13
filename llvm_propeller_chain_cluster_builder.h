#ifndef AUTOFDO_LLVM_PROPELLER_CHAIN_CLUSTER_BUILDER_H_
#define AUTOFDO_LLVM_PROPELLER_CHAIN_CLUSTER_BUILDER_H_

#include <algorithm>
#include <utility>
#include <vector>

#include "llvm_propeller_node_chain.h"
#include "llvm_propeller_options.pb.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/functional/function_ref.h"

namespace devtools_crosstool_autofdo {

// Represents an ordered cluster of chains.
class ChainCluster {
 public:
  explicit ChainCluster(std::unique_ptr<const NodeChain> chain)
      : id_(chain->id()), size_(chain->size_), freq_(chain->freq_) {
    chains_.push_back(std::move(chain));
  }

  // ChainCluster is a moveonly type.
  ChainCluster(ChainCluster &&) = default;
  ChainCluster &operator=(ChainCluster &&) = default;

  ChainCluster(const ChainCluster &) = delete;
  ChainCluster &operator=(const ChainCluster &) = delete;

  const std::vector<std::unique_ptr<const NodeChain>> &chains() {
    return chains_;
  }

  // Returns the total binary size of the cluster.
  uint64_t size() const { return size_; }

  // Returns the total frquency of the cluster.
  uint64_t freq() const { return freq_; }

  // Returns the unique identifier for this cluster.
  uint64_t id() const { return id_; }

  // Returns the execution density for this cluster.
  double exec_density() const {
    return static_cast<double>(freq_) / std::max(size_, 1ul);
  }

  // Merges the chains in `other` cluster into `this` cluster. `other`
  // ChainCluster will be consumed by this call.
  void MergeWith(ChainCluster other) {
    absl::c_move(other.chains_, std::back_inserter(chains_));
    this->freq_ += other.freq_;
    this->size_ += other.size_;
  }

  // Iterates over all nodes in this cluster (in order) and applies the given
  // `func` on every node.
  void VisitEachNodeRef(absl::FunctionRef<void(const CFGNode &)> func) const {
    for (const std::unique_ptr<const NodeChain> &chain : chains_)
      chain->VisitEachNodeRef(func);
  }

 private:
  // The chains in this cluster in the merged order.
  std::vector<std::unique_ptr<const NodeChain>> chains_ = {};

  // Unique id of the cluster.
  uint64_t id_ = 0;

  // Total size of the cluster.
  uint64_t size_ = 0;

  // Total frequency of the cluster.
  uint64_t freq_ = 0;
};

class ChainClusterBuilder {
 public:
  // ChainClusterBuilder constructor: This initializes one cluster per each
  // chain and transfers the ownership of the NodeChain pointer to their
  // associated clusters.
  explicit ChainClusterBuilder(
      const devtools_crosstool_autofdo::PropellerCodeLayoutParameters
          &code_layout_params,
      std::vector<std::unique_ptr<const NodeChain>> chains)
      : code_layout_params_(code_layout_params) {
    for (auto &chain : chains) {
      const NodeChain *chain_ptr = chain.get();
      // Transfer the ownership of chains to clusters.
      auto cluster = std::make_unique<ChainCluster>(std::move(chain));
      chain_to_cluster_map_.emplace(chain_ptr, cluster.get());
      auto cluster_id = cluster->id();
      bool inserted = clusters_.emplace(cluster_id, std::move(cluster)).second;
      CHECK(inserted) << "Duplicate cluster id: " << cluster_id << ".";
    }
    chains.clear();
  }

  // Builds and returns the clusters of chains.
  // This function builds clusters of node chains according to the
  // call-chain-clustering algorithm[1] and returns them in a vector. After this
  // is called, all clusters are moved to the vector and the `clusters_`
  // map becomes empty.
  // [1] https://dl.acm.org/doi/10.5555/3049832.3049858
  std::vector<std::unique_ptr<const ChainCluster>> BuildClusters() &&;

  // Finds the most frequent predecessor cluster of `chain` and merges it with
  // `chain`'s cluster.
  void MergeWithBestPredecessorCluster(const NodeChain &chain);

  // Merges `right_cluster` into `left_cluster`. This call consumes
  // `right_cluster`.
  void MergeClusters(ChainCluster &left_cluster, ChainCluster right_cluster);

 private:
  devtools_crosstool_autofdo::PropellerCodeLayoutParameters code_layout_params_;

  // All clusters currently in process.
  absl::flat_hash_map<uint64_t, std::unique_ptr<const ChainCluster>> clusters_;

  // This maps every chain to its containing cluster.
  absl::flat_hash_map<const NodeChain *, ChainCluster *> chain_to_cluster_map_;
};

}  // namespace devtools_crosstool_autofdo

#endif  //  AUTOFDO_LLVM_PROPELLER_CHAIN_CLUSTER_BUILDER_H_
