#include "llvm_propeller_profile_writer.h"

#if defined(HAVE_LLVM)

#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "llvm_propeller_whole_program_info.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

namespace devtools_crosstool_autofdo {

using ::devtools_crosstool_autofdo::PropellerOptions;
using ::llvm::Optional;
using ::llvm::StringRef;

absl::Status GeneratePropellerProfiles(const PropellerOptions &opts) {
  std::unique_ptr<PropellerProfWriter> writer =
      PropellerProfWriter::Create(opts);
  if (!writer)
    return absl::InternalError("Failed to create PropellerProfWriter object");

  const devtools_crosstool_autofdo::CodeLayoutResult layout_per_function =
      devtools_crosstool_autofdo::CodeLayout(
          opts.code_layout_params(), writer->whole_program_info()->GetHotCfgs())
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
  LOG(INFO) << "Parsed " << stats_.perf_file_parsed << " profiles.";
  LOG(INFO) << "Total " << stats_.binary_mmap_num << " binary mmaps.";
  LOG(INFO) << "Total  "
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
  const double intra_score_percent_change =
      100 * (static_cast<double>(stats_.optimized_intra_score) /
                 stats_.original_intra_score -
             1);
  LOG(INFO) << absl::StreamFormat(
      "Changed intra-function (ext-tsp) score by %+.1f%% from %llu to %llu.",
      intra_score_percent_change, stats_.original_intra_score,
      stats_.optimized_intra_score);

  const double inter_score_percent_change =
      100 * (static_cast<double>(stats_.optimized_inter_score) /
                 stats_.original_inter_score -
             1);
  LOG(INFO) << absl::StreamFormat(
      "Changed inter-function (ext-tsp) score by %+.1f%% from %llu to %llu.",
      inter_score_percent_change, stats_.original_inter_score,
      stats_.optimized_inter_score);
}

bool PropellerProfWriter::Write(const CodeLayoutResult &layout_cluster_info) {
  // Find total number of clusters
  unsigned total_clusters = 0;
  for (auto &elem : layout_cluster_info)
    total_clusters += elem.second.clusters.size();

  // Allocate the symbol order vector
  std::vector<std::pair<llvm::SmallVector<StringRef, 3>, Optional<unsigned>>>
      symbol_order(total_clusters);
  // Allocate the cold symbol order vector equally sized as
  // layout_cluster_info, as there is one cold cluster per function.
  std::vector<CFGNode *> cold_symbol_order(layout_cluster_info.size());

  std::ofstream out_stream(options_.cluster_out_name());
  // TODO(b/160339651): Remove this in favour of structured format in LLVM code.
  for (const auto &[unused, func_layout_info] : layout_cluster_info) {
    stats_.original_intra_score += func_layout_info.original_score.intra_score;
    stats_.optimized_intra_score +=
        func_layout_info.optimized_score.intra_score;
    stats_.original_inter_score +=
        func_layout_info.original_score.inter_out_score;
    stats_.optimized_inter_score +=
        func_layout_info.optimized_score.inter_out_score;

    // Print all alias names of the function, separated by '/'.
    out_stream << "!" << llvm::join(func_layout_info.cfg->names(), "/") << "\n";

    if (options_.verbose_cluster_output()) {
      // Print the layout score for intra-function and inter-function edges
      // involving this function. This information allows us to study the impact
      // on layout score on each individual function.
      out_stream << absl::StreamFormat(
          "#ext-tsp score: [intra: %llu -> %llu] [inter: %llu -> %llu]\n",
          func_layout_info.original_score.intra_score,
          func_layout_info.optimized_score.intra_score,
          func_layout_info.original_score.inter_out_score,
          func_layout_info.optimized_score.inter_out_score);
    }
    auto &clusters = func_layout_info.clusters;
    for (unsigned cluster_id = 0; cluster_id < clusters.size(); ++cluster_id) {
      auto &cluster = clusters[cluster_id];
      // If a cluster starts with zero BB index (function entry basic block),
      // the function name is sufficient for section ordering. Otherwise,
      // the cluster number is required.
      symbol_order[cluster.layout_index] =
          std::pair<llvm::SmallVector<StringRef, 3>, Optional<unsigned>>(
              func_layout_info.cfg->names_, cluster.bb_indexes.front() == 0
                                                ? Optional<unsigned>()
                                                : cluster_id);
      for (unsigned i = 0; i < cluster.bb_indexes.size(); ++i)
        out_stream << (i ? " " : "!!") << cluster.bb_indexes[i];
      out_stream << "\n";
    }
    cold_symbol_order[func_layout_info.cold_cluster_layout_index] =
        func_layout_info.cfg->coalesced_cold_node_;
  }

  std::ofstream symorder_stream(options_.symbol_order_out_name());
  for (const auto &[func_names, cluster_id] : symbol_order) {
    // Print the symbol names corresponding to every function name alias. This
    // guarantees we get the right order regardless of which function name is
    // picked by the compiler.
    for (auto &func_name : func_names) {
      symorder_stream << func_name.str();
      if (cluster_id.hasValue())
        symorder_stream << ".__part." << cluster_id.getValue();
      symorder_stream << "\n";
    }
  }

  // Insert the .cold symbols for cold parts of hot functions.
  for (CFGNode *cold_node : cold_symbol_order) {
    // The cold node could be null if all BBs in the CFG are hot.
    if (cold_node == nullptr) continue;
    for (auto &func_name : cold_node->cfg()->names()) {
      symorder_stream << func_name.str();
      if (!cold_node->is_entry()) symorder_stream << ".cold";
    }
    symorder_stream << "\n";
  }

  stats_ += whole_program_info_->stats();
  PrintStats();
  return true;
}
}  // namespace devtools_crosstool_autofdo
#endif
