#include "llvm_propeller_profile_writer.h"

#if defined(HAVE_LLVM)

#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_options.h"
#include "llvm_propeller_statistics.h"
#include "llvm_propeller_whole_program_info.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

namespace devtools_crosstool_autofdo {

using ::llvm::Optional;
using ::llvm::StringRef;

absl::Status GeneratePropellerProfiles(
    const std::string &binary_file_name, const std::string &perf_file_name,
    const std::string &cluster_output_file_name,
    const std::string &order_output_file_name,
    const std::string &profiled_binary_name, bool ignore_build_id) {
  PropellerOptions opts;
  opts.WithBinaryName(binary_file_name)
      .WithPerfName(perf_file_name)
      .WithOutName(cluster_output_file_name)
      .WithSymOrderName(order_output_file_name)
      .WithProfiledBinaryName(profiled_binary_name)
      .WithIgnoreBuildId(ignore_build_id);
  std::unique_ptr<PropellerProfWriter> writer =
      PropellerProfWriter::Create(opts);
  if (!writer)
    return absl::InternalError("Failed to create PropellerProfWriter object");

  const devtools_crosstool_autofdo::CodeLayoutResult layout_per_function =
      devtools_crosstool_autofdo::CodeLayout(
          writer->whole_program_info()->GetHotCfgs())
          .OrderAll();
  if (!writer->Write(layout_per_function))
    return absl::InternalError("Failed to compute code layout result");
  return absl::OkStatus();
}

// Load binary file content into memory and set up binary_is_pie_ flag.
std::unique_ptr<PropellerProfWriter> PropellerProfWriter::Create(
    const PropellerOptions &options) {
  std::unique_ptr<AbstractPropellerWholeProgramInfo> whole_program_info =
      PropellerWholeProgramInfo::Create(options);
  if (!whole_program_info) {
    // Error message already logged in PropellerWholeProgramInfo::Create.
    return nullptr;
  }
  if (!whole_program_info->CreateCfgs()) return nullptr;
  return std::unique_ptr<PropellerProfWriter>(
      new PropellerProfWriter(options, std::move(whole_program_info)));
}

void PropellerProfWriter::PrintStats() const {
  LOG(INFO) << "Total " << stats_.binary_mmap_num << " binary mmaps.";
  LOG(INFO) << "Totaly  "
            << CommaStyleNumberFormatter(stats_.br_counters_accumulated)
            << " br entries accumulated.";
  LOG(INFO) << "Created " << CommaStyleNumberFormatter(stats_.syms_created)
            << " syms.";
  LOG(INFO) << "Created " << CommaStyleNumberFormatter(stats_.cfgs_created)
            << " cfgs.";
  LOG(INFO) << "Created " << CommaStyleNumberFormatter(stats_.nodes_created)
            << " nodes.";
  LOG(INFO) << "Created " << CommaStyleNumberFormatter(stats_.edges_created)
            << " edges.";

  if (stats_.edges_with_same_src_sink_but_different_type)
    LOG(WARNING) << "Found edges with same source&sink but different type "
                 << CommaStyleNumberFormatter(
                        stats_.edges_with_same_src_sink_but_different_type);
  if (stats_.dropped_symbols)
    LOG(WARNING) << "Dropped "
                 << CommaStyleNumberFormatter(stats_.dropped_symbols)
                 << " symbols.";
  if (stats_.duplicate_symbols)
    LOG(WARNING) << "Duplicate symbols: "
                 << CommaStyleNumberFormatter(stats_.duplicate_symbols)
                 << " symbols.";
  if (stats_.bbaddrmap_function_does_not_have_symtab_entry)
    LOG(WARNING) << "Dropped "
                 << CommaStyleNumberFormatter(
                        stats_.bbaddrmap_function_does_not_have_symtab_entry)
                 << " bbaddrmap entries, because they do not have "
                    "corresponding symbols in "
                    "binary symtab.";
  const double score_percent_change =
      100 * (static_cast<double>(stats_.optimized_intra_score) /
                 stats_.original_intra_score -
             1);
  LOG(INFO) << absl::StreamFormat(
      "Changed layout (ext-tsp) score by %+.1f%% from %llu to %llu.",
      score_percent_change, stats_.original_intra_score,
      stats_.optimized_intra_score);
}

bool PropellerProfWriter::Write(const CodeLayoutResult &layout_cluster_info) {
  // Find total number of clusters
  unsigned total_clusters = 0;
  for (auto &elem : layout_cluster_info)
    total_clusters += elem.second.clusters.size();

  // Allocate the symbol order vector
  std::vector<std::pair<StringRef, Optional<unsigned>>> symbol_order(
      total_clusters);
  // Allocate the cold symbol order vector equally sized as
  // layout_cluster_info, as there is one cold cluster per function.
  std::vector<CFGNode *> cold_symbol_order(layout_cluster_info.size());

  std::ofstream out_stream(options_.out_name);
  // TODO(b/160339651): Remove this in favour of structured format in LLVM code.
  for (const auto &[unused, func_layout_info] : layout_cluster_info) {
    stats_.original_intra_score += func_layout_info.original_intra_score;
    stats_.optimized_intra_score += func_layout_info.optimized_intra_score;

    StringRef func_name = func_layout_info.cfg->GetPrimaryName();
    // Print all alias names of the function, separated by '/'.
    out_stream << "!" << llvm::join(func_layout_info.cfg->names_, "/") << "\n";
    auto &clusters = func_layout_info.clusters;
    for (unsigned cluster_id = 0; cluster_id < clusters.size(); ++cluster_id) {
      auto &cluster = clusters[cluster_id];
      // If a cluster starts with zero BB index (function entry basic block),
      // the function name is sufficient for section ordering. Otherwise,
      // the cluster number is required.
      symbol_order[cluster.layout_index] =
          std::pair<StringRef, Optional<unsigned>>(
              func_name, cluster.bb_indexes.front() == 0 ? Optional<unsigned>()
                                                         : cluster_id);
      for (unsigned i = 0; i < cluster.bb_indexes.size(); ++i)
        out_stream << (i ? " " : "!!") << cluster.bb_indexes[i];
      out_stream << "\n";
    }
    cold_symbol_order[func_layout_info.cold_cluster_layout_index] =
        func_layout_info.cfg->coalesced_cold_node_;
  }

  std::ofstream symorder_stream(options_.symorder_name);
  for (const auto &[func_name, cluster_id] : symbol_order) {
    symorder_stream << func_name.str();
    if (cluster_id.hasValue()) symorder_stream << "." << cluster_id.getValue();
    symorder_stream << "\n";
  }

  // Insert the .cold symbols for cold parts of hot functions.
  for (CFGNode *cold_node : cold_symbol_order) {
    // The cold node could be null if all BBs in the CFG are hot.
    if (cold_node == nullptr) continue;
    symorder_stream << cold_node->cfg_->GetPrimaryName().str();
    if (!cold_node->is_entry()) symorder_stream << ".cold";
    symorder_stream << "\n";
  }

  stats_ += whole_program_info_->stats();
  PrintStats();
  return true;
}
}  // namespace devtools_crosstool_autofdo
#endif
