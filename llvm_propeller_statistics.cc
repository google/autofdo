#include "llvm_propeller_statistics.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_chain_merge_order.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/str_join.h"

namespace devtools_crosstool_autofdo {

std::string PropellerStats::CodeLayoutStats::DebugString() const {
  const double intra_score_percent_change =
      100 * (optimized_intra_score / original_intra_score - 1);

  const double inter_score_percent_change =
      100 * (optimized_inter_score / original_inter_score - 1);

  return absl::StrJoin(
      {absl::StrCat(
           "Merge order stats: ",
           absl::StrJoin(n_assemblies_by_merge_order, ", ",
                         [](std::string *out,
                            const std::pair<ChainMergeOrder, int> &entry) {
                           absl::StrAppend(out, "[",
                                           GetMergeOrderName(entry.first), ":",
                                           entry.second, "]");
                         })),
       absl::StrCat("Initial chains stats: single-node chains: [",
                    n_single_node_chains, "] multi-node chains: [",
                    n_multi_node_chains, "]"),
       absl::StrFormat(
           "Changed inter-function (ext-tsp) score by %+.1f%% from %f to %f.",
           inter_score_percent_change, original_inter_score,
           optimized_inter_score),
       absl::StrFormat(
           "Changed intra-function (ext-tsp) score by %+.1f%% from %f to %f",
           intra_score_percent_change, original_intra_score,
           optimized_intra_score)},
      "\n");
}

std::string PropellerStats::DisassemblyStats::Stat::DebugString() const {
  return absl::StrFormat("absolute: %d / weighted: %d", absolute, weighted);
}

std::string PropellerStats::DisassemblyStats::DebugString() const {
  return absl::StrFormat(
      "Disassembly stats:\nCould not disassemble: %s\nMay affect control flow: "
      "%s\nCan not affect control flow: %s",
      could_not_disassemble.DebugString(),
      may_affect_control_flow.DebugString(),
      cant_affect_control_flow.DebugString());
}

std::string PropellerStats::DebugString() const {
  int64_t edges_created = total_edges_created();
  int64_t total_edge_weight = total_edge_weight_created();

  std::vector<std::string> stat_lines = {
      absl::StrCat("Parsed ", perf_file_parsed, " profiles."),
      absl::StrCat("Total ", binary_mmap_num, " binary mmaps."),
      absl::StrCat("Total ", br_counters_accumulated,
                   " br entries accumulated."),
      absl::StrCat(hot_functions,
                   " hot functions (alias included) found in profiles."),
      absl::StrCat(hot_basic_blocks, " hot basic blocks found in profiles."),
      absl::StrCat("Created ", cfgs_created, " cfgs."),
      absl::StrCat("Created ", nodes_created, " nodes."),
      absl::StrCat(cfgs_with_hot_landing_pads, " cfgs have hot landing pads."),
      absl::StrCat(hot_empty_basic_blocks, " hot blocks have zero size."),

      absl::StrCat(
          "Created ", edges_created, " edges: {",
          absl::StrJoin(
              edges_created_by_kind, ", ",
              [edges_created](std::string *out,
                              const std::pair<CFGEdge::Kind, int64_t> &entry) {
                return absl::StrAppend(
                    out,
                    absl::StrFormat("%s: %.2f%%",
                                    CFGEdge::GetCfgEdgeKindString(entry.first),
                                    entry.second * 100.0 / edges_created));
              }),
          "}."),

      absl::StrCat(
          "Profiled ", total_edge_weight, " total edge weight: {",
          absl::StrJoin(total_edge_weight_by_kind, ", ",
                        [total_edge_weight](
                            std::string *out,
                            const std::pair<CFGEdge::Kind, int64_t> &entry) {
                          return absl::StrAppend(
                              out,
                              absl::StrFormat(
                                  "%s: %.2f%%",
                                  CFGEdge::GetCfgEdgeKindString(entry.first),
                                  entry.second * 100.0 / total_edge_weight));
                        }),
          "}.")};

  if (edges_with_same_src_sink_but_different_type) {
    stat_lines.push_back(absl::StrCat(
        "Found edges with same source and sink but different type ",
        edges_with_same_src_sink_but_different_type));
  }
  if (duplicate_symbols) {
    stat_lines.push_back(
        absl::StrCat("Duplicate symbols: ", duplicate_symbols, " symbols."));
  }
  if (bbaddrmap_function_does_not_have_symtab_entry) {
    stat_lines.push_back(
        absl::StrCat("Dropped ", bbaddrmap_function_does_not_have_symtab_entry,
                     " bbaddrmap entries, because they do not have "
                     "corresponding symbols in "
                     "binary symtab."));
  }
  stat_lines.push_back(code_layout_stats.DebugString());
  stat_lines.push_back(disassembly_stats.DebugString());
  return absl::StrJoin(stat_lines, "\n");
}
}  // namespace devtools_crosstool_autofdo
