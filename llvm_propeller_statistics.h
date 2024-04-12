#ifndef AUTOFDO_LLVM_PROPELLER_STATISTICS_H_
#define AUTOFDO_LLVM_PROPELLER_STATISTICS_H_

#include <cstdint>
#include <string>
#include <utility>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_merge_order.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"

namespace devtools_crosstool_autofdo {

struct PropellerStats {
  struct CodeLayoutStats {
    double original_intra_score = 0;
    double optimized_intra_score = 0;
    double original_inter_score = 0;
    double optimized_inter_score = 0;

    // Number of assemblies applied by NodeChainBuilder separated by
    // merge_order. These are the assemblies with which
    // NodeChainBuilder::MergeChains(assembly) has been called.
    absl::flat_hash_map<ChainMergeOrder, int> n_assemblies_by_merge_order;
    // Number of initial single-node chains.
    int n_single_node_chains = 0;
    // Number of initial multi-node chains.
    int n_multi_node_chains = 0;

    void operator+=(const CodeLayoutStats &other) {
      original_intra_score += other.original_intra_score;
      optimized_intra_score += other.optimized_intra_score;
      original_inter_score += other.original_inter_score;
      optimized_inter_score += other.optimized_inter_score;
      for (const auto &[merge_order, count] :
           other.n_assemblies_by_merge_order) {
        n_assemblies_by_merge_order[merge_order] += count;
      }
      n_single_node_chains += other.n_single_node_chains;
      n_multi_node_chains += other.n_multi_node_chains;
    }

    std::string DebugString() const;
  };

  struct DisassemblyStats {
    struct Stat {
      // Counts the absolute number of instructions satisfying a condition.
      // If an instruction is a source of many LBR branches, it's still counted
      // only once.
      int64_t absolute = 0;

      // Counts the weighted number of instructions satisfying a condition.
      // Each instruction is counted as many times as it appears as the source
      // of an LBR branch.
      int64_t weighted = 0;

      std::string DebugString() const;
      void Increment(int64_t weight) {
        weighted += weight;
        ++absolute;
      }

      void operator+=(const Stat &other) {
        absolute += other.absolute;
        weighted += other.weighted;
      }
    };

    Stat could_not_disassemble;
    Stat may_affect_control_flow;
    Stat cant_affect_control_flow;

    void operator+=(const DisassemblyStats &other) {
      could_not_disassemble += other.could_not_disassemble;
      may_affect_control_flow += other.may_affect_control_flow;
      cant_affect_control_flow += other.cant_affect_control_flow;
    }

    std::string DebugString() const;
  };

  struct ProfileStats {
    int binary_mmap_num = 0;
    int perf_file_parsed = 0;
    uint64_t br_counters_accumulated = 0;

    void operator+=(const ProfileStats &other) {
      br_counters_accumulated += other.br_counters_accumulated;
      binary_mmap_num += other.binary_mmap_num;
      perf_file_parsed += other.perf_file_parsed;
    }

    std::string DebugString() const;
  };

  struct CfgStats {
    uint64_t edges_with_same_src_sink_but_different_type = 0;
    uint64_t cfgs_created = 0;
    // Number of CFGs which have hot landing pads.
    int cfgs_with_hot_landing_pads = 0;
    uint64_t nodes_created = 0;
    absl::flat_hash_map<CFGEdge::Kind, int64_t> edges_created_by_kind;
    absl::flat_hash_map<CFGEdge::Kind, int64_t> total_edge_weight_by_kind;
    int hot_basic_blocks = 0;
    int hot_empty_basic_blocks = 0;

    int64_t total_edges_created() const {
      return absl::c_accumulate(
          edges_created_by_kind, int64_t{0},
          [](int64_t psum, const std::pair<CFGEdge::Kind, int64_t> &entry) {
            return psum + entry.second;
          });
    }

    int64_t total_edge_weight_created() const {
      return absl::c_accumulate(
          total_edge_weight_by_kind, int64_t{0},
          [](int64_t psum, const std::pair<CFGEdge::Kind, int64_t> &entry) {
            return psum + entry.second;
          });
    }

    void operator+=(const CfgStats &other) {
      edges_with_same_src_sink_but_different_type +=
          other.edges_with_same_src_sink_but_different_type;
      cfgs_created += other.cfgs_created;
      cfgs_with_hot_landing_pads += other.cfgs_with_hot_landing_pads;
      nodes_created += other.nodes_created;
      for (const auto &[kind, count] : other.edges_created_by_kind) {
        edges_created_by_kind[kind] += count;
      }
      for (const auto &[kind, count] : other.total_edge_weight_by_kind) {
        total_edge_weight_by_kind[kind] += count;
      }
      hot_basic_blocks += other.hot_basic_blocks;
      hot_empty_basic_blocks += other.hot_empty_basic_blocks;
    }

    std::string DebugString() const;
  };

  struct BbAddrMapStats {
    uint64_t duplicate_symbols = 0;
    uint64_t bbaddrmap_function_does_not_have_symtab_entry = 0;
    uint64_t hot_functions = 0;

    void operator+=(const BbAddrMapStats &other) {
      duplicate_symbols += other.duplicate_symbols;
      bbaddrmap_function_does_not_have_symtab_entry +=
          other.bbaddrmap_function_does_not_have_symtab_entry;
      hot_functions += other.hot_functions;
    }

    std::string DebugString() const;
  };

  struct CloningStats {
    int paths_cloned = 0;
    int bbs_cloned = 0;
    int bytes_cloned = 0;
    double score_gain = 0;

    void operator+=(const CloningStats &other) {
      paths_cloned += other.paths_cloned;
      bbs_cloned += other.bbs_cloned;
      bytes_cloned += other.bytes_cloned;
      score_gain += other.score_gain;
    }

    std::string DebugString() const;
  };

  BbAddrMapStats bbaddrmap_stats;

  ProfileStats profile_stats;
  DisassemblyStats disassembly_stats;

  CfgStats cfg_stats;
  CodeLayoutStats code_layout_stats;
  CloningStats cloning_stats;

  void operator+=(const PropellerStats &other) {
    bbaddrmap_stats += other.bbaddrmap_stats;
    profile_stats += other.profile_stats;
    disassembly_stats += other.disassembly_stats;
    cfg_stats += other.cfg_stats;
    code_layout_stats += other.code_layout_stats;
    cloning_stats += other.cloning_stats;
  }

  std::string DebugString() const;
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_STATISTICS_H_
