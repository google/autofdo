#ifndef AUTOFDOLLVM_PROPELLER_MOCK_PROGRAM_CFG_BUILDER_H_
#define AUTOFDOLLVM_PROPELLER_MOCK_PROGRAM_CFG_BUILDER_H_

#include <memory>
#include <string>
#include <utility>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_cfg_testutil.h"
#include "llvm_propeller_program_cfg.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "llvm/Support/Allocator.h"

namespace devtools_crosstool_autofdo {

// Represents a whole program cfg constructed from a test protobuf.
class ProtoProgramCfg {
 public:
  ProtoProgramCfg(
      std::unique_ptr<llvm::BumpPtrAllocator> bump_ptr_allocator,
      absl::flat_hash_map<int, std::unique_ptr<ControlFlowGraph>> cfgs)
      : program_cfg_(std::move(cfgs)),
        bump_ptr_allocator_(std::move(bump_ptr_allocator)) {}

  const ProgramCfg &program_cfg() const { return program_cfg_; }

 private:
  const ProgramCfg program_cfg_;
  std::unique_ptr<llvm::BumpPtrAllocator> bump_ptr_allocator_;
};

// Constructs and returns a `ProtoProgramCfg` from a a protobuf file stored in
// `path_to_cfg_proto`.
absl::StatusOr<std::unique_ptr<ProtoProgramCfg>> BuildFromCfgProtoPath(
    const std::string &path_to_cfg_proto);

// Constructs and returns a `ProgramCfg` from a the given `multi_cfg_arg`.
std::unique_ptr<ProgramCfg> BuildFromCfgArg(MultiCfgArg multi_cfg_arg);
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDOLLVM_PROPELLER_MOCK_PROGRAM_CFG_BUILDER_H_
