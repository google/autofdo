#ifndef AUTOFDO_LLVM_PROPELLER_PROGRAM_CFG_PROTO_BUILDER_H_
#define AUTOFDO_LLVM_PROPELLER_PROGRAM_CFG_PROTO_BUILDER_H_

#include <memory>
#include <string>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_cfg.pb.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"  // for "google::protobuf::io::FileInputStream"

namespace devtools_crosstool_autofdo {

// Builds a `ProgramCfgPb` and writes it into a file.
class ProgramCfgProtoBuilder {
 public:
  // Prepares protobuf output stream.
  bool InitWriter(const std::string &out_name);
  // Adds CFGs to protobuf container.
  void AddCfgs(
      const absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> &cfgs);
  // Writes everything and close.
  bool WriteAndClose();

  // Reads from proto text file.
  bool Read(const std::string &in_name);

  const ProgramCfgPb &program_cfg_pb() const { return program_cfg_pb_; }

 private:
  void PopulateControlFlowGraphPb(ControlFlowGraphPb *cfgpb,
                                  const ControlFlowGraph &cfg);
  void PopulateCFGNodePb(CFGNodePb *nodepb, const CFGNode &node);
  void PopulateCFGEdgePb(CFGEdgePb *edgepb, const CFGEdge &edge);

 private:
  // Protobuf output file name.
  std::string protobuf_out_name_;
  // Protobuf output stream.
  std::unique_ptr<google::protobuf::io::FileOutputStream> protobuf_out_;
  // Protobuf container.
  ProgramCfgPb program_cfg_pb_;
};

}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_PROGRAM_CFG_PROTO_BUILDER_H_
