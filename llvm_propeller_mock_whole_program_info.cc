#include "llvm_propeller_mock_whole_program_info.h"

#include <fcntl.h>  // for "O_RDONLY"

#include <string>
#include <utility>

#include "llvm_propeller_cfg.pb.h"
#include "llvm_propeller_options.pb.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"  // for "typename google::protobuf::io::FileInputStream"
#include "google/protobuf/text_format.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"

namespace devtools_crosstool_autofdo {

static std::unique_ptr<CFGNode> CreateNodeFromNodePb(const CFGNodePb &nodepb,
                                                     ControlFlowGraph *cfg) {
  return std::make_unique<CFGNode>(nodepb.symbol_ordinal(), 0,
                                   nodepb.bb_index(), nodepb.size(), cfg,
                                   nodepb.freq());
}

bool MockPropellerWholeProgramInfo::CreateCfgs() {
  std::string perf_name = options_.perf_names(0);
  int fd = open(perf_name.c_str(), O_RDONLY);
  if (fd == -1) {
    LOG(ERROR) << "Failed to open and read '" << perf_name << "'.";
    return false;
  }
  typename google::protobuf::io::FileInputStream fis(fd);
  fis.SetCloseOnDelete(true);
  LOG(INFO) << "Reading from '" << perf_name << "'.";
  if (!google::protobuf::TextFormat::Parse(&fis, &propeller_pb_)) return false;
  CreateCfgsFromProtobuf();
  return true;
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
    std::vector<SymbolEntry *> symbols;
    for (const auto &nodepb : cfgpb.node()) {
      std::unique_ptr<CFGNode> node = CreateNodeFromNodePb(nodepb, cfg.get());
      ordinal_to_node_map.try_emplace(node->symbol_ordinal(), node.get());
      if (node->freq()) cfg->hot_tag_ = true;
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
      cfg->CreateEdge(from_n, to_n, edgepb.weight(),
                      convertFromPBKind(edgepb.kind()));
      ++stats_.edges_created;
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
