#include "llvm_propeller_chain_cluster_builder.h"

#include <utility>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_node_chain.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/numeric/int128.h"

namespace devtools_crosstool_autofdo {

namespace {
// We omit edges if their weight is less than
// 1/kHotEdgeRelativeFrequencyThreshold of their sink.
constexpr int kHotEdgeRelativeFrequencyThreshold = 10;
// We avoid clustering if it reduces the density by more than
// 1/kDensityDegradationThreshold.
constexpr absl::int128 kExecutionDensityDegradationThreshold = 8;
// We avoid clustering chains with less than kChainExecutionDensityThreshold
// execution density.
constexpr double kChainExecutionDensityThreshold = 0.005;
}  // namespace

void ChainClusterBuilder::MergeWithBestPredecessorCluster(
    const NodeChain &chain) {
  ChainCluster *cluster = chain_to_cluster_map_.at(&chain);
  // If the cluster is too big, avoid merging as it is unlikely to have
  // significant benefit.
  if (cluster->size() > code_layout_params_.cluster_merge_size_threshold())
    return;

  // Create a map to compute the total incoming edge weight to `cluster`
  // from each other cluster.
  absl::flat_hash_map<ChainCluster *, uint64_t> weight_from;

  // TODO(b/231473745): Consider edges to all nodes for inter-procedural layout.
  CHECK(chain.GetFirstNode()->is_entry())
      << "First node in the chain is not a function entry block.";
  chain.GetFirstNode()->ForEachInEdgeRef([&](CFGEdge &edge) {
    if (!edge.IsCall()) return;
    // Omit the edge if it's cold relative to the sink.
    if (edge.weight() * kHotEdgeRelativeFrequencyThreshold <
        edge.sink()->freq()) {
      return;
    }
    const NodeChain &src_chain = GetNodeChain(edge.src());
    if (src_chain.id() == chain.id()) return;
    ChainCluster *src_cluster = chain_to_cluster_map_.at(&src_chain);
    if (src_cluster->id() == cluster->id()) return;
    // Ignore clusters that are larger than the threshold.
    if (src_cluster->size() >
        code_layout_params_.cluster_merge_size_threshold()) {
      return;
    }
    // Avoid merging if the predecessor cluster's density would degrade by
    // more than 1/kDensityDegradationThreshold by the merge.
    if (kExecutionDensityDegradationThreshold * src_cluster->size() *
            (cluster->freq() + src_cluster->freq()) <
        src_cluster->freq() * (cluster->size() + src_cluster->size())) {
      return;
    }
    weight_from[src_cluster] += edge.weight();
  });

  if (weight_from.empty()) return;

  // Find the predecessor cluster with the largest (total) incoming edge weight.
  ChainCluster *best_pred_cluster =
      absl::c_max_element(weight_from, [](const auto &p1, const auto &p2) {
        return std::forward_as_tuple(p1.second, p1.first->id()) <
               std::forward_as_tuple(p2.second, p2.first->id());
      })->first;

  MergeClusters(*best_pred_cluster, std::move(*cluster));
}

// Merges `right_clusters` into and to the right side of `left_cluster` and
// removes it from `clusters_`.
void ChainClusterBuilder::MergeClusters(ChainCluster &left_cluster,
                                        ChainCluster right_cluster) {
  // Update chain to cluster mapping for chains in right_cluster, as they will
  // be placed in left_cluster.
  for (const std::unique_ptr<const NodeChain> &chain : right_cluster.chains())
    chain_to_cluster_map_[chain.get()] = &left_cluster;

  auto right_cluster_it = clusters_.find(right_cluster.id());
  // Join right_cluster into left_cluster.
  left_cluster.MergeWith(std::move(right_cluster));

  // Delete the defunct right_cluster.
  clusters_.erase(right_cluster_it);
}

std::vector<std::unique_ptr<const ChainCluster>>
ChainClusterBuilder::BuildClusters() && {
  std::vector<std::unique_ptr<const ChainCluster>> built_clusters;
  if (!code_layout_params_.call_chain_clustering()) {
    // Simply order the chains consistently with the original ordering.
    for (auto &[unused, cluster] : clusters_)
      built_clusters.push_back(std::move(cluster));
    absl::c_sort(built_clusters, [](const auto &lhs, const auto &rhs) {
      return lhs->id() < rhs->id();
    });
    return built_clusters;
  }

  std::vector<const NodeChain *> chains_sorted_by_incoming_weight;
  for (const auto &[chain, unused] : chain_to_cluster_map_)
    chains_sorted_by_incoming_weight.push_back(chain);

  // Sort chains in decreasing order of their total incoming edge weights.
  // TODO(b/231473745): Compute total incoming edge weight for inter-procedural
  // layout.
  absl::c_sort(chains_sorted_by_incoming_weight, [](const NodeChain *lhs,
                                                    const NodeChain *rhs) {
    return std::forward_as_tuple(-lhs->GetFirstNode()->freq(), lhs->id()) <
           std::forward_as_tuple(-rhs->GetFirstNode()->freq(), rhs->id());
  });

  for (const NodeChain *chain : chains_sorted_by_incoming_weight) {
    // Do not merge clusters when the execution density is negligible.
    if (chain->exec_density() < kChainExecutionDensityThreshold) continue;
    MergeWithBestPredecessorCluster(*chain);
  }

  for (auto &[unused_id, cluster] : clusters_)
    built_clusters.push_back(std::move(cluster));
  // Order final clusters in decreasing order of their execution density.
  absl::c_sort(built_clusters, [](const auto &lhs, const auto &rhs) {
    return std::forward_as_tuple(-lhs->exec_density(), lhs->id()) <
           std::forward_as_tuple(-rhs->exec_density(), rhs->id());
  });
  return built_clusters;
}

}  // namespace devtools_crosstool_autofdo
