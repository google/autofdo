#include "llvm_propeller_chain_cluster_builder.h"

#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_node_chain.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/types/span.h"

namespace devtools_crosstool_autofdo {

namespace {
// We omit edges if their weight is less than
// 1/kHotEdgeRelativeFrequencyThreshold of their sink.
constexpr int kHotEdgeRelativeFrequencyThreshold = 10;
// We avoid clustering if it reduces the density by more than
// 1/kDensityDegradationThreshold.
constexpr int64_t kExecutionDensityDegradationThreshold = 8;
// We avoid clustering chains with less than kChainExecutionDensityThreshold
// execution density.
constexpr double kChainExecutionDensityThreshold = 0.005;

absl::flat_hash_map<const CFGNode *, const NodeChain *> BuildNodeToChainMap(
    absl::Span<const std::unique_ptr<const NodeChain>> chains) {
  absl::flat_hash_map<const CFGNode *, const NodeChain *> result;
  for (auto &chain : chains) {
    chain->VisitEachNodeRef(
        [&](const CFGNode &node) { result.emplace(&node, chain.get()); });
  }
  return result;
}
}  // namespace

ChainClusterBuilder::ChainClusterBuilder(
    const PropellerCodeLayoutParameters &code_layout_params,
    std::vector<std::unique_ptr<const NodeChain>> chains)
    : code_layout_params_(code_layout_params),
      node_to_chain_map_(BuildNodeToChainMap(chains)) {
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

void ChainClusterBuilder::MergeWithBestPredecessorCluster(
    const NodeChain &chain) {
  ChainCluster *cluster = chain_to_cluster_map_.at(&chain);
  // If the cluster is too big, avoid merging as it is unlikely to have
  // significant benefit.
  if (cluster->size() > code_layout_params_.cluster_merge_size_threshold())
    return;

  // Create a map to compute the total incoming edge weight to `cluster`
  // from each other cluster.
  absl::flat_hash_map<ChainCluster *, int64_t> weight_from;

  // Update the `weight_from` edges by visiting all incoming edges to the given
  // `node`.
  auto inspect_in_edges = [&](const CFGNode &node) {
    node.ForEachInEdgeRef([&](const CFGEdge &edge) {
      // Omit return edges since optimizing them does not improve performance.
      if (edge.IsReturn()) return;
      if (edge.inter_section()) return;
      // Omit the edge if it's cold relative to the sink.
      if (edge.weight() == 0 ||
          edge.weight() * kHotEdgeRelativeFrequencyThreshold <
              edge.sink()->CalculateFrequency()) {
        return;
      }
      const NodeChain &src_chain = *node_to_chain_map_.at(edge.src());
      // Omit intra-chain edges.
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
          static_cast<int64_t>(src_cluster->freq()) *
              (cluster->size() + src_cluster->size())) {
        return;
      }
      weight_from[src_cluster] += edge.weight();
    });
  };

  if (!code_layout_params_.inter_function_reordering()) {
    CHECK(chain.GetFirstNode()->is_entry())
        << "First node in the chain for function #" << *chain.function_index()
        << " is not an entry block.";
    inspect_in_edges(*chain.GetFirstNode());
  } else {
    chain.VisitEachNodeRef(inspect_in_edges);
  }

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

  // Total incoming edge weight to each chain excluding return edges and
  // intra-chain edges.
  absl::flat_hash_map<const NodeChain *, int64_t> weight_to;
  for (const auto &[chain, unused] : chain_to_cluster_map_) {
    int64_t weight = 0;
    auto chain_id = chain->id();
    chain->VisitEachNodeRef([&](const CFGNode &node) {
      node.ForEachInEdgeRef([&](CFGEdge &edge) {
        if (edge.weight() == 0 || edge.IsReturn() || edge.inter_section() ||
            node_to_chain_map_.at(edge.src())->id() == chain_id) {
          return;
        }
        weight += edge.weight();
      });
    });
    if (weight != 0) weight_to.insert({chain, weight});
  }

  std::vector<const NodeChain *> chains_sorted_by_incoming_weight;
  for (const auto &[chain, unused] : weight_to)
    chains_sorted_by_incoming_weight.push_back(chain);

  // Sort chains in decreasing order of their total incoming edge weights.
  absl::c_sort(chains_sorted_by_incoming_weight,
               [&weight_to](const NodeChain *lhs, const NodeChain *rhs) {
                 return std::forward_as_tuple(-weight_to[lhs], lhs->id()) <
                        std::forward_as_tuple(-weight_to[rhs], rhs->id());
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
