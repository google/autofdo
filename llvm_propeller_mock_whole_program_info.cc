#include "llvm_propeller_mock_whole_program_info.h"

#include <fcntl.h>  // for "O_RDONLY"

#include "llvm_propeller_options.pb.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"  // for "typename google::protobuf::io::FileInputStream"
#include "google/protobuf/text_format.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"

namespace devtools_crosstool_autofdo {

static void PopulateNodeFromNodePb(CFGNode *node, const CFGNodePb &nodepb) {
  node->freq_ = nodepb.freq();
  node->size_ = nodepb.size();
  node->symbol_ordinal_ = nodepb.symbol_ordinal();
  node->bb_index_ = nodepb.bb_index();
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

// Create control flow graph from protobuf and delete protobuf afterwards.
void MockPropellerWholeProgramInfo::CreateCfgsFromProtobuf() {
  bump_ptr_allocator_ = std::make_unique<llvm::BumpPtrAllocator>();
  string_saver_ = std::make_unique<llvm::StringSaver>(*bump_ptr_allocator_);
  std::map<uint64_t, CFGNode *> ordinal_to_node_map;
  // Now construct the CFG.
  for (const auto &cfgpb : propeller_pb_.cfg()) {
    ControlFlowGraph *cfg = new ControlFlowGraph();
    for (const auto &name : cfgpb.name())
      cfg->names_.emplace_back(string_saver_->save(name));
    ++stats_.cfgs_created;
    std::vector<SymbolEntry *> symbols;
    for (const auto &nodepb : cfgpb.node()) {
      CFGNode *node = new CFGNode(cfg);
      PopulateNodeFromNodePb(node, nodepb);
      ordinal_to_node_map.try_emplace(node->symbol_ordinal_, node);
      cfg->nodes_.emplace(node);
      if (node->freq_) cfg->hot_tag_ = true;
    }
    stats_.nodes_created += cfg->nodes_.size();
    cfgs_.emplace(cfg->names_.front(), cfg);
  }

  // Now construct the edges
  auto help_construct_edge = [&ordinal_to_node_map, this](auto &edges) {
    for (const auto &edgepb : edges) {
      auto *from_n = ordinal_to_node_map[edgepb.source()];
      auto *to_n = ordinal_to_node_map[edgepb.sink()];
      CHECK(from_n);
      CHECK(to_n);
      auto *cfg = from_n->cfg_;
      CHECK(cfg);
      cfg->CreateEdge(from_n, to_n, edgepb.weight(),
                      static_cast<CFGEdge::Info>(edgepb.info()));
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
