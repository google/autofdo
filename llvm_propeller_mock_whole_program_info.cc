#include "llvm_propeller_mock_whole_program_info.h"

#include <fcntl.h>  // for "O_RDONLY"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_cfg.pb.h"
#include "llvm_propeller_options.pb.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"  // for "typename google::protobuf::io::FileInputStream"
#include "google/protobuf/text_format.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"

namespace devtools_crosstool_autofdo {

static std::unique_ptr<CFGNode> CreateNodeFromNodePb(const CFGNodePb &nodepb,
                                                     ControlFlowGraph *cfg) {
  return std::make_unique<CFGNode>(
      /*symbol_ordinal=*/nodepb.symbol_ordinal(), /*addr=*/0,
      /*bb_index=*/nodepb.bb_index(), /*size=*/nodepb.size(),
      /*is_landing_pad=*/nodepb.is_landing_pad(), /*cfg=*/cfg,
      /*freq=*/nodepb.freq());
}

absl::Status MockPropellerWholeProgramInfo::CreateCfgs(
    CfgCreationMode cfg_creation_mode) {
  std::string perf_name = options_.perf_names(0);
  int fd = open(perf_name.c_str(), O_RDONLY);
  if (fd == -1) {
    return absl::FailedPreconditionError(
        absl::StrFormat("Failed to open and read profile '%s'.", perf_name));
  }
  typename google::protobuf::io::FileInputStream fis(fd);
  fis.SetCloseOnDelete(true);
  LOG(INFO) << "Reading from '" << perf_name << "'.";
  if (!google::protobuf::TextFormat::Parse(&fis, &propeller_pb_)) {
    return absl::InternalError(
        absl::StrFormat("Unable to parse profile '%s'", perf_name));
  }
  CreateCfgsFromProtobuf();
  return absl::OkStatus();
}

static CFGEdge::Kind convertFromPBKind(CFGEdgePb::Kind kindpb) {
  switch (kindpb) {
    case CFGEdgePb::BRANCH_OR_FALLTHROUGH:
      return CFGEdge::Kind::kBranchOrFallthough;
    case CFGEdgePb::CALL:
      return CFGEdge::Kind::kCall;
    case CFGEdgePb::RETURN:
      return CFGEdge::Kind::kRet;
  }
}
// Create control flow graph from protobuf and delete protobuf afterwards.
void MockPropellerWholeProgramInfo::CreateCfgsFromProtobuf() {
  bump_ptr_allocator_ = std::make_unique<llvm::BumpPtrAllocator>();
  string_saver_ = std::make_unique<llvm::StringSaver>(*bump_ptr_allocator_);
  std::map<uint64_t, CFGNode *> ordinal_to_node_map;
  // Now construct the CFG.
  for (const auto &cfgpb : propeller_pb_.cfg()) {
    llvm::SmallVector<llvm::StringRef, 3> names;
    names.reserve(cfgpb.name().size());
    for (const auto &name : cfgpb.name())
      names.emplace_back(string_saver_->save(name));
    auto cfg = std::make_unique<ControlFlowGraph>(std::move(names));
    ++stats_.cfgs_created;
    for (const auto &nodepb : cfgpb.node()) {
      std::unique_ptr<CFGNode> node = CreateNodeFromNodePb(nodepb, cfg.get());
      ordinal_to_node_map.try_emplace(node->symbol_ordinal(), node.get());
      if (node->freq()) {
        cfg->hot_tag_ = true;
        if (node->is_landing_pad()) ++cfg->n_hot_landing_pads_;
      }
      if (node->is_landing_pad()) ++cfg->n_landing_pads_;
      cfg->nodes_.emplace(std::move(node));
    }
    stats_.nodes_created += cfg->nodes_.size();
    cfgs_.emplace(cfg->names().front(), std::move(cfg));
  }

  // Now construct the edges
  auto help_construct_edge = [&ordinal_to_node_map, this](auto &edges) {
    for (const auto &edgepb : edges) {
      auto *from_n = ordinal_to_node_map[edgepb.source()];
      auto *to_n = ordinal_to_node_map[edgepb.sink()];
      CHECK(from_n);
      CHECK(to_n);
      auto *cfg = from_n->cfg();
      CHECK(cfg);
      auto edge_kind = convertFromPBKind(edgepb.kind());
      cfg->CreateEdge(from_n, to_n, edgepb.weight(),
                      edge_kind);
      ++stats_.edges_created_by_kind[edge_kind];
      stats_.total_edge_weight_by_kind[edge_kind] += edgepb.weight();
    }
  };
  for (const auto &cfgpb : propeller_pb_.cfg()) {
    for (const auto &nodepb : cfgpb.node()) {
      help_construct_edge(nodepb.intra_outs());
      help_construct_edge(nodepb.inter_outs());
    }
  }
  propeller_pb_.Clear();
}

}  // namespace devtools_crosstool_autofdo
