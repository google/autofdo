#ifndef AUTOFDO_LLVM_PROPELLER_MOCK_WHOLE_PROGRAM_INFO_H_
#define AUTOFDO_LLVM_PROPELLER_MOCK_WHOLE_PROGRAM_INFO_H_

#if defined(HAVE_LLVM)

#include <memory>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_cfg.pb.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"

namespace devtools_crosstool_autofdo {

class MockPropellerWholeProgramInfo final : public AbstractPropellerWholeProgramInfo {
 public:
  explicit MockPropellerWholeProgramInfo(const PropellerOptions &options)
      : AbstractPropellerWholeProgramInfo(options) {}

  ~MockPropellerWholeProgramInfo() final {}

  // "only_for_hot_functions": see comments in
  // "AbstractPropellerWholeProgramInfo".
  absl::Status CreateCfgs(CfgCreationMode cfg_creation_mode) override;

 private:
  void CreateCfgsFromProtobuf();

  // When we construct Symbols/CFGs from protobuf, bump_ptr_allocator_ and
  // string_saver_ are used to keep all the string content. (Whereas in case of
  // constructing from binary files, the strings are kept in
  // binary_file_content.)
  std::unique_ptr<llvm::BumpPtrAllocator> bump_ptr_allocator_;
  std::unique_ptr<llvm::StringSaver> string_saver_;

  // Protobuf container.
  PropellerPb propeller_pb_;
};

}  // namespace devtools_crosstool_autofdo

#endif
#endif  // AUTOFDO_LLVM_PROPELLER_MOCK_WHOLE_PROGRAM_INFO_H_
