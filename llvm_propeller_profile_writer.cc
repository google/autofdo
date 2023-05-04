#include "llvm_propeller_profile_writer.h"

#if defined(HAVE_LLVM)

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "llvm_propeller_whole_program_info.h"
#include "status_consumer_registry.h"
#include "status_provider.h"
#include "third_party/abseil/absl/memory/memory.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/str_join.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

namespace {
using ::devtools_crosstool_autofdo::FunctionClusterInfo;
void DumpCfgs(
    const std::vector<FunctionClusterInfo> &all_functions_cluster_info,
    absl::string_view cfg_dump_dir_name) {
  // Create the cfg dump directory and the cfg index file.
  std::filesystem::create_directories(cfg_dump_dir_name);
  auto cfg_index_file =
      std::filesystem::path(cfg_dump_dir_name) / "cfg-index.txt";
  std::ofstream cfg_index_os(cfg_index_file, std::ofstream::out);
  CHECK(cfg_index_os.good())
      << "Failed to open " << cfg_index_file << " for writing.";
  cfg_index_os << absl::StrJoin({"Function.Name", "Function.Address", "N_Nodes",
                                 "N_Clusters", "Original.ExtTSP.Score",
                                 "Optimized.ExtTSP.Score"},
                                " ")
               << "\n";

  for (const FunctionClusterInfo &func_layout_info :
       all_functions_cluster_info) {
    // Dump hot cfgs into the given directory.
    auto func_addr_str = absl::StrCat(
        "0x", absl::Hex(func_layout_info.cfg->GetEntryNode()->addr()));
    cfg_index_os << func_layout_info.cfg->GetPrimaryName().str() << " "
                 << func_addr_str << " " << func_layout_info.cfg->nodes().size()
                 << " " << func_layout_info.clusters.size() << " "
                 << func_layout_info.original_score.intra_score << " "
                 << func_layout_info.optimized_score.intra_score << "\n";

    // Use the address of the function as the CFG filename for uniqueness.
    auto cfg_dump_file = std::filesystem::path(cfg_dump_dir_name) /
                         absl::StrCat(func_addr_str, ".dot");
    std::ofstream cfg_dump_os(cfg_dump_file, std::ofstream::out);
    CHECK(cfg_dump_os.good())
        << "Failed to open " << cfg_dump_file << " for writing.";

    absl::flat_hash_map<int, int> layout_index_map;
    for (auto &cluster : func_layout_info.clusters)
      for (int bbi = 0; bbi < cluster.bb_indexes.size(); ++bbi)
        layout_index_map.insert(
            {cluster.bb_indexes[bbi], cluster.layout_index + bbi});

    func_layout_info.cfg->WriteDotFormat(cfg_dump_os, layout_index_map);
  }
}
}  // namespace

namespace devtools_crosstool_autofdo {

using ::devtools_crosstool_autofdo::PropellerOptions;
using ::llvm::StringRef;

absl::Status GeneratePropellerProfiles(const PropellerOptions &opts) {
  return GeneratePropellerProfiles(
      opts, std::make_unique<FilePerfDataProvider>(std::vector<std::string>(
                opts.perf_names().begin(), opts.perf_names().end())));
}

absl::Status GeneratePropellerProfiles(
    const PropellerOptions &opts,
    std::unique_ptr<PerfDataProvider> perf_data_provider) {
  MultiStatusProvider main_status("generate propeller profiles");
  auto *frontend_status = new MultiStatusProvider("frontend");
  main_status.AddStatusProvider(50, absl::WrapUnique(frontend_status));
  auto *codelayout_status = new DefaultStatusProvider("codelayout");
  main_status.AddStatusProvider(50, absl::WrapUnique(codelayout_status));
  auto *writefile_status = new DefaultStatusProvider("result_writer");
  main_status.AddStatusProvider(1, absl::WrapUnique(writefile_status));
  // RegistryStopper calls "Stop" on all registered status consumers before
  // exiting. Because the stopper "stops" consumers that have a reference to
  // main_status, we must declared "stopper" after "main_status" so "stopper" is
  // deleted before "main_status".
  struct RegistryStopper {
    ~RegistryStopper() {
      if (registry) registry->Stop();
    }
    StatusConsumerRegistry *registry = nullptr;
  } stopper;
  if (opts.http()) {
    stopper.registry =
        &devtools_crosstool_autofdo::StatusConsumerRegistry::GetInstance();
    stopper.registry->Start(main_status);
  }
  std::unique_ptr<PropellerProfWriter> writer = PropellerProfWriter::Create(
      opts, std::move(perf_data_provider), frontend_status);
  if (!writer)
    return absl::InternalError("Failed to create PropellerProfWriter object");
  frontend_status->SetDone();

  const std::vector<FunctionClusterInfo> layout_per_function =
      devtools_crosstool_autofdo::CodeLayout(
          opts.code_layout_params(), writer->whole_program_info()->GetHotCfgs())
          .OrderAll();
  codelayout_status->SetDone();
  if (!writer->Write(layout_per_function))
    return absl::InternalError("Failed to compute code layout result");
  writefile_status->SetDone();
  return absl::OkStatus();
}

// Load binary file content into memory and set up binary_is_pie_ flag.
std::unique_ptr<PropellerProfWriter> PropellerProfWriter::Create(
    const PropellerOptions &options, MultiStatusProvider *frontend_status) {
  return Create(options,
                std::make_unique<FilePerfDataProvider>(std::vector<std::string>(
                    options.perf_names().begin(), options.perf_names().end())),
                frontend_status);
}

std::unique_ptr<PropellerProfWriter> PropellerProfWriter::Create(
    const PropellerOptions &options,
    std::unique_ptr<PerfDataProvider> perf_data_provider,
    MultiStatusProvider *frontend_status) {
  std::unique_ptr<AbstractPropellerWholeProgramInfo> whole_program_info =
      PropellerWholeProgramInfo::Create(options, std::move(perf_data_provider),
                                        frontend_status);
  if (!whole_program_info) {
    // Error message already logged in PropellerWholeProgramInfo::Create.
    return nullptr;
  }
  absl::Status create_cfgs_status =
      whole_program_info->CreateCfgs(CfgCreationMode::kOnlyHotFunctions);
  if (!create_cfgs_status.ok()) {
    LOG(ERROR) << create_cfgs_status;
    return nullptr;
  }
  return std::unique_ptr<PropellerProfWriter>(
      new PropellerProfWriter(options, std::move(whole_program_info)));
}

void PropellerProfWriter::PrintStats() const {
  LOG(INFO) << "Parsed " << stats_.perf_file_parsed << " profiles.";
  LOG(INFO) << "Total " << stats_.binary_mmap_num << " binary mmaps.";
  LOG(INFO) << "Total  "
            << CommaStyleNumberFormatter(stats_.br_counters_accumulated)
            << " br entries accumulated.";
  LOG(INFO) << CommaStyleNumberFormatter(stats_.hot_functions)
            << " hot functions (alias included) found in profiles.";
  LOG(INFO) << "Created " << CommaStyleNumberFormatter(stats_.cfgs_created)
            << " cfgs.";
  LOG(INFO) << "Created " << CommaStyleNumberFormatter(stats_.nodes_created)
            << " nodes.";
  LOG(INFO) << CommaStyleNumberFormatter(stats_.cfgs_with_hot_landing_pads)
            << " cfgs have hot landing pads.";

  int64_t edges_created = stats_.total_edges_created();
  LOG(INFO) << "Created " << CommaStyleNumberFormatter(edges_created)
            << " edges: {"
            << absl::StrJoin(
                   stats_.edges_created_by_kind, ", ",
                   [edges_created](
                       std::string *out,
                       const std::pair<CFGEdge::Kind, int64_t> &entry) {
                     return absl::StrAppend(
                         out, absl::StrFormat(
                                  "%s: %.2f%%",
                                  CFGEdge::GetCfgEdgeKindString(entry.first),
                                  entry.second * 100.0 / edges_created));
                   })
            << "}.";

  int64_t total_edge_weight = stats_.total_edge_weight_created();
  LOG(INFO) << "Profiled " << CommaStyleNumberFormatter(total_edge_weight)
            << " total edge weight: {"
            << absl::StrJoin(
                   stats_.total_edge_weight_by_kind, ", ",
                   [total_edge_weight](
                       std::string *out,
                       const std::pair<CFGEdge::Kind, int64_t> &entry) {
                     return absl::StrAppend(
                         out, absl::StrFormat(
                                  "%s: %.2f%%",
                                  CFGEdge::GetCfgEdgeKindString(entry.first),
                                  entry.second * 100.0 / total_edge_weight));
                   })
            << "}.";

  if (stats_.edges_with_same_src_sink_but_different_type)
    LOG(WARNING) << "Found edges with same source&sink but different type "
                 << CommaStyleNumberFormatter(
                        stats_.edges_with_same_src_sink_but_different_type);
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

bool PropellerProfWriter::Write(
    const std::vector<FunctionClusterInfo> &all_functions_cluster_info) {
  // Find total number of clusters.
  unsigned total_clusters = 0;
  for (const auto &func_cluster_info : all_functions_cluster_info)
    total_clusters += func_cluster_info.clusters.size();

  // Allocate the symbol order vector
  std::vector<std::pair<llvm::SmallVector<StringRef, 3>, std::optional<unsigned>>>
      symbol_order(total_clusters);
  // Allocate the cold symbol order vector equally sized as
  // all_functions_cluster_info, as there is (at most) one cold cluster per
  // function.
  std::vector<const FunctionClusterInfo *> cold_symbol_order(
      all_functions_cluster_info.size());
  std::ofstream out_stream(options_.cluster_out_name());
  // TODO(b/160339651): Remove this in favour of structured format in LLVM code.
  for (const FunctionClusterInfo &func_layout_info :
       all_functions_cluster_info) {
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
      // Print out the frequency of the function entry node.
      out_stream << absl::StreamFormat(
          "#entry-freq %llu\n", func_layout_info.cfg->GetEntryNode()->freq());
    }
    auto &clusters = func_layout_info.clusters;
    for (unsigned cluster_id = 0; cluster_id < clusters.size(); ++cluster_id) {
      auto &cluster = clusters[cluster_id];
      // If a cluster starts with zero BB index (function entry basic block),
      // the function name is sufficient for section ordering. Otherwise,
      // the cluster number is required.
      symbol_order[cluster.layout_index] =
          std::pair<llvm::SmallVector<StringRef, 3>, std::optional<unsigned>>(
              func_layout_info.cfg->names_, cluster.bb_indexes.front() == 0
                                                ? std::optional<unsigned>()
                                                : cluster_id);
      for (int bbi = 0; bbi < cluster.bb_indexes.size(); ++bbi)
        out_stream << (bbi ? " " : "!!") << cluster.bb_indexes[bbi];
      out_stream << "\n";
    }

    cold_symbol_order[func_layout_info.cold_cluster_layout_index] =
        &func_layout_info;
  }

  if (options_.has_cfg_dump_dir_name())
    DumpCfgs(all_functions_cluster_info, options_.cfg_dump_dir_name());

  std::ofstream symorder_stream(options_.symbol_order_out_name());
  for (const auto &[func_names, cluster_id] : symbol_order) {
    // Print the symbol names corresponding to every function name alias. This
    // guarantees we get the right order regardless of which function name is
    // picked by the compiler.
    for (auto &func_name : func_names) {
      symorder_stream << func_name.str();
      if (cluster_id.has_value())
        symorder_stream << ".__part." << *cluster_id;
      symorder_stream << "\n";
    }
  }

  // Insert the .cold symbols for cold parts of hot functions.
  for (const FunctionClusterInfo * cluster_info : cold_symbol_order) {
    // The cold node should not be emitted if all basic blocks appear in the
    // clusters.
    int num_bbs_in_clusters = 0;
    for (const FunctionClusterInfo::BBCluster &cluster : cluster_info->clusters)
      num_bbs_in_clusters += cluster.bb_indexes.size();
    if (num_bbs_in_clusters == cluster_info->cfg->nodes_.size()) continue;
    // Check if the function entry is in the clusters. The entry node always
    // begins its cluster. So this simply checks the first node in every
    // cluster.
    bool entry_is_in_clusters = absl::c_any_of(cluster_info->clusters,
                         [](const FunctionClusterInfo::BBCluster &cluster) {
                           return cluster.bb_indexes.front() == 0;
                         });
    for (auto &func_name : cluster_info->cfg->names()) {
      symorder_stream << func_name.str();
      // If the entry node is not in clusters, function name can serve as the
      // cold symbol name. So we don't need the ".cold" suffix.
      if (entry_is_in_clusters) symorder_stream << ".cold";
      symorder_stream << "\n";
    }
  }

  stats_ += whole_program_info_->stats();
  PrintStats();
  return true;
}
}  // namespace devtools_crosstool_autofdo
#endif
