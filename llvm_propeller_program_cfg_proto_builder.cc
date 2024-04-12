#include "llvm_propeller_program_cfg_proto_builder.h"

#include <fcntl.h>

#include <memory>
#include <string>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_cfg.pb.h"
#include "base/logging.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/text_format.h"

namespace devtools_crosstool_autofdo {

bool ProgramCfgProtoBuilder::InitWriter(const std::string &out_name) {
  if (out_name.empty()) return true;
  protobuf_out_name_ = out_name;
  CHECK(!protobuf_out_);
  int fd = open(out_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
  if (fd == -1) {
    LOG(ERROR) << "Failed to create/open '" << out_name << "'.";
    return false;
  }
  protobuf_out_ = std::make_unique<google::protobuf::io::FileOutputStream>(fd);
  return true;
}

bool ProgramCfgProtoBuilder::WriteAndClose() {
  CHECK(protobuf_out_.get());
  if (!google::protobuf::TextFormat::Print(program_cfg_pb_, protobuf_out_.get())) {
    LOG(ERROR) << "Failed to write protobuf to '" << protobuf_out_name_ << ".";
    return false;
  }
  if (!protobuf_out_->Close()) {
    LOG(ERROR) << "Failed to close '" << protobuf_out_name_ << "'.";
    return false;
  }
  return true;
}

// Add CFGs to protobuf.
void ProgramCfgProtoBuilder::AddCfgs(
    const absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> &cfgs) {
  for (auto &[function_index, cfg] : cfgs)
    PopulateControlFlowGraphPb(program_cfg_pb_.add_cfg(), *cfg);
}

void ProgramCfgProtoBuilder::PopulateControlFlowGraphPb(
    ControlFlowGraphPb *cfgpb, const ControlFlowGraph &cfg) {
  for (const auto &name : cfg.names()) cfgpb->add_name(std::string(name));
  cfgpb->set_function_index(cfg.function_index());
  cfgpb->set_section_name(std::string(cfg.section_name()));
  for (const auto &n : cfg.nodes()) PopulateCFGNodePb(cfgpb->add_node(), *n);
}

void ProgramCfgProtoBuilder::PopulateCFGNodePb(CFGNodePb *nodepb,
                                               const CFGNode &node) {
  nodepb->set_size(node.size());
  nodepb->set_bb_id(node.bb_id());
  CHECK_EQ(node.clone_number(), 0)
      << " Unsupported non-zero clone number: " << node.clone_number();
  CFGNodePb::MetadataPb &metadata_pb = *nodepb->mutable_metadata();
  metadata_pb.set_can_fallthrough(node.can_fallthrough());
  metadata_pb.set_has_return(node.has_return());
  metadata_pb.set_has_tail_call(node.has_tail_call());
  metadata_pb.set_is_landing_pad(node.is_landing_pad());

  for (const CFGEdge *edge : node.intra_outs())
    PopulateCFGEdgePb(nodepb->add_out_edges(), *edge);
}

void ProgramCfgProtoBuilder::PopulateCFGEdgePb(CFGEdgePb *edgepb,
                                               const CFGEdge &edge) {
  edgepb->mutable_sink()->set_bb_index(edge.sink()->intra_cfg_id().bb_index);
  edgepb->mutable_sink()->set_function_index(edge.sink()->function_index());

  switch (edge.kind()) {
    case CFGEdge::Kind::kBranchOrFallthough:
      edgepb->set_kind(
          ::devtools_crosstool_autofdo::CFGEdgePb::BRANCH_OR_FALLTHROUGH);
      break;
    case CFGEdge::Kind::kCall:
      edgepb->set_kind(::devtools_crosstool_autofdo::CFGEdgePb::CALL);
      break;
    case CFGEdge::Kind::kRet:
      edgepb->set_kind(::devtools_crosstool_autofdo::CFGEdgePb::RETURN);
      break;
  }
  edgepb->set_weight(edge.weight());
}

bool ProgramCfgProtoBuilder::Read(const std::string &in_name) {
  int fd = open(in_name.c_str(), O_RDONLY);
  if (fd == -1) {
    LOG(ERROR) << "Failed to open and read '" << in_name << "'.";
    return false;
  }
  google::protobuf::io::FileInputStream fis(fd);
  fis.SetCloseOnDelete(true);
  LOG(INFO) << "Reading from '" << in_name << "'.";
  return google::protobuf::TextFormat::Parse(&fis, &program_cfg_pb_);
}

}  // namespace devtools_crosstool_autofdo
