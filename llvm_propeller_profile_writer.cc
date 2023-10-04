#include "llvm_propeller_profile_writer.h"

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_function_cluster_info.h"
#include "llvm_propeller_profile.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/log/check.h"

#if defined(HAVE_LLVM)

#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_options.pb.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/str_join.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace devtools_crosstool_autofdo {

namespace {
void DumpCfgs(const PropellerProfile &profile,
              absl::string_view cfg_dump_dir_name) {
  // Create the cfg dump directory and the cfg index file.
  llvm::sys::fs::create_directory(cfg_dump_dir_name);
  llvm::SmallString<100> cfg_index_file_vec(cfg_dump_dir_name.begin(),
                                            cfg_dump_dir_name.end());
  llvm::sys::path::append(cfg_index_file_vec, "cfg-index.txt");
  std::string cfg_index_file(cfg_index_file_vec.str());
  std::ofstream cfg_index_os(cfg_index_file, std::ofstream::out);
  CHECK(cfg_index_os.good())
      << "Failed to open " << cfg_index_file << " for writing.";
  cfg_index_os << absl::StrJoin({"Function.Name", "Function.Address", "N_Nodes",
                                 "N_Clusters", "Original.ExtTSP.Score",
                                 "Optimized.ExtTSP.Score"},
                                " ")
               << "\n";

  for (const auto &[section_name, section_function_cluster_info] :
       profile.functions_cluster_info_by_section_name) {
    for (const FunctionClusterInfo &func_cluster_info :
         section_function_cluster_info) {
      const ControlFlowGraph *cfg =
          profile.program_cfg->GetCfgByIndex(func_cluster_info.function_index);
      CHECK_NE(cfg, nullptr);
      // Dump hot cfgs into the given directory.
      auto func_addr_str =
          absl::StrCat("0x", absl::Hex(cfg->GetEntryNode()->addr()));
      cfg_index_os << cfg->GetPrimaryName().str() << " " << func_addr_str << " "
                   << cfg->nodes().size() << " "
                   << func_cluster_info.clusters.size() << " "
                   << func_cluster_info.original_score.intra_score << " "
                   << func_cluster_info.optimized_score.intra_score << "\n";

      // Use the address of the function as the CFG filename for uniqueness.
      llvm::SmallString<100> cfg_dump_file_vec(cfg_dump_dir_name.begin(),
                                               cfg_dump_dir_name.end());
      llvm::sys::path::append(cfg_dump_file_vec,
                              absl::StrCat(func_addr_str, ".dot"));
      std::string cfg_dump_file(cfg_dump_file_vec.str());
      std::ofstream cfg_dump_os(cfg_dump_file, std::ofstream::out);
      CHECK(cfg_dump_os.good())
          << "Failed to open " << cfg_dump_file << " for writing.";

      absl::flat_hash_map<CFGNode::IntraCfgId, int> layout_index_map;
      for (auto &cluster : func_cluster_info.clusters)
        for (int bbi = 0; bbi < cluster.full_bb_ids.size(); ++bbi)
          layout_index_map.insert({cluster.full_bb_ids[bbi].intra_cfg_id,
                                   cluster.layout_index + bbi});

      cfg->WriteDotFormat(cfg_dump_os, layout_index_map);
    }
  }
}

// Dumps the intra-function edge profile of `cfg` into `out`.
// For each CFGNode, it prints out a line in the format of
// "#<bb>:<bb_freq> <succ_bb_1>:<edge_freq_1> <succ_bb_2>:<edge_freq_2> ..."
// which starts first with the bb id and frequency of that node, followed by the
// successors and their edge frequencies. Please note that the edge weights
// may not precisely add up to the node frequency.
void DumpCfgProfile(const ControlFlowGraph &cfg, std::ofstream &out) {
  cfg.ForEachNodeRef([&](const CFGNode &node) {
    int node_frequency = node.CalculateFrequency();
    out << "#cfg-prof " << node.bb_id() << ":" << node_frequency;
    node.ForEachOutEdgeRef([&](const CFGEdge &edge) {
      if (!edge.IsBranchOrFallthrough()) return;
      out << " " << edge.sink()->bb_id() << ":" << edge.weight();
    });
    out << "\n";
  });
}
}  // namespace

void PropellerProfileWriter::Write(const PropellerProfile &profile) const {
  std::ofstream cc_profile_os(options_.cluster_out_name());
  std::ofstream ld_profile_os(options_.symbol_order_out_name());
  // TODO(b/160339651): Remove this in favour of structured format in LLVM code.
  for (const auto &[section_name, section_function_cluster_info] :
       profile.functions_cluster_info_by_section_name) {
    if (options_.verbose_cluster_output())
      cc_profile_os << "#section " << section_name.str() << "\n";
    // Find total number of clusters.
    unsigned total_clusters = 0;
    for (const auto &func_cluster_info : section_function_cluster_info)
      total_clusters += func_cluster_info.clusters.size();

    // Allocate the symbol order vector
    std::vector<std::pair<llvm::SmallVector<llvm::StringRef, 3>,
                          std::optional<unsigned>>>
        symbol_order(total_clusters);
    // Allocate the cold symbol order vector equally sized as
    // function_cluster_info, as there is (at most) one cold cluster per
    // function.
    std::vector<const FunctionClusterInfo *> cold_symbol_order(
        section_function_cluster_info.size());
    for (const FunctionClusterInfo &func_layout_info :
         section_function_cluster_info) {
      const ControlFlowGraph *cfg =
          profile.program_cfg->GetCfgByIndex(func_layout_info.function_index);
      CHECK_NE(cfg, nullptr);
      // Print all alias names of the function, separated by '/'.
      cc_profile_os << "!" << llvm::join(cfg->names(), "/");
      // Print module names.
      if (cfg->module_name().has_value())
        cc_profile_os << " M=" << cfg->module_name().value().str();
      cc_profile_os << "\n";
      // Print cloning paths.
      for (const std::vector<CFGNode::FullIntraCfgId> &clone_path :
           cfg->clone_paths()) {
        cc_profile_os << "!!!"
                      << absl::StrJoin(clone_path, " ",
                                       [](std::string *out,
                                          const CFGNode::FullIntraCfgId &id) {
                                         absl::StrAppend(out, id.bb_id);
                                       })
                      << "\n";
      }
      if (options_.verbose_cluster_output()) {
        // Print the layout score for intra-function and inter-function edges
        // involving this function. This information allows us to study the
        // impact on layout score on each individual function.
        cc_profile_os << absl::StreamFormat(
            "#ext-tsp score: [intra: %f -> %f] [inter: %f -> %f]\n",
            func_layout_info.original_score.intra_score,
            func_layout_info.optimized_score.intra_score,
            func_layout_info.original_score.inter_out_score,
            func_layout_info.optimized_score.inter_out_score);
        // Print out the frequency of the function entry node.
        cc_profile_os << absl::StreamFormat(
            "#entry-freq %llu\n", cfg->GetEntryNode()->CalculateFrequency());
      }
      auto &clusters = func_layout_info.clusters;
      for (unsigned cluster_id = 0; cluster_id < clusters.size();
           ++cluster_id) {
        auto &cluster = clusters[cluster_id];
        // If a cluster starts with zero BB index (function entry basic block),
        // the function name is sufficient for section ordering. Otherwise,
        // the cluster number is required.
        symbol_order[cluster.layout_index] =
            std::pair<llvm::SmallVector<llvm::StringRef, 3>,
                      std::optional<unsigned>>(
                cfg->names(),
                cluster.full_bb_ids.front().intra_cfg_id.bb_index == 0
                    ? std::optional<unsigned>()
                    : cluster_id);
        for (int bbi = 0; bbi < cluster.full_bb_ids.size(); ++bbi) {
          const auto &full_bb_id = cluster.full_bb_ids[bbi];
          cc_profile_os << (bbi ? " " : "!!") << full_bb_id.bb_id;
          if (full_bb_id.intra_cfg_id.clone_number != 0)
            cc_profile_os << "." << full_bb_id.intra_cfg_id.clone_number;
        }
        cc_profile_os << "\n";
      }

      // Dump the edge profile for this CFG if requested.
      if (options_.verbose_cluster_output())
        DumpCfgProfile(*cfg, cc_profile_os);

      cold_symbol_order[func_layout_info.cold_cluster_layout_index] =
          &func_layout_info;
    }

    for (const auto &[func_names, cluster_id] : symbol_order) {
      // Print the symbol names corresponding to every function name alias. This
      // guarantees we get the right order regardless of which function name is
      // picked by the compiler.
      for (auto &func_name : func_names) {
        ld_profile_os << func_name.str();
        if (cluster_id.has_value())
          ld_profile_os << ".__part." << cluster_id.value();
        ld_profile_os << "\n";
      }
    }

    // Insert the .cold symbols for cold parts of hot functions.
    for (const FunctionClusterInfo *cluster_info : cold_symbol_order) {
      const ControlFlowGraph *cfg =
          profile.program_cfg->GetCfgByIndex(cluster_info->function_index);
      CHECK_NE(cfg, nullptr);
      // The cold node should not be emitted if all basic blocks appear in the
      // clusters.
      int num_bbs_in_clusters = 0;
      for (const FunctionClusterInfo::BBCluster &cluster :
           cluster_info->clusters)
        num_bbs_in_clusters += cluster.full_bb_ids.size();
      if (num_bbs_in_clusters == cfg->nodes().size()) continue;
      // Check if the function entry is in the clusters. The entry node always
      // begins its cluster. So this simply checks the first node in every
      // cluster.
      bool entry_is_in_clusters = absl::c_any_of(
          cluster_info->clusters,
          [](const FunctionClusterInfo::BBCluster &cluster) {
            return cluster.full_bb_ids.front().intra_cfg_id.bb_index == 0;
          });
      for (auto &func_name : cfg->names()) {
        ld_profile_os << func_name.str();
        // If the entry node is not in clusters, function name can serve as the
        // cold symbol name. So we don't need the ".cold" suffix.
        if (entry_is_in_clusters) ld_profile_os << ".cold";
        ld_profile_os << "\n";
      }
    }
  }
  if (options_.has_cfg_dump_dir_name())
    DumpCfgs(profile, options_.cfg_dump_dir_name());
}
}  // namespace devtools_crosstool_autofdo
#endif
