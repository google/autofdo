#ifndef AUTOFDOMINI_DISASSEMBLER_H_
#define AUTOFDOMINI_DISASSEMBLER_H_

#include <cstdint>
#include <memory>

#include "third_party/abseil/absl/base/nullability.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Object/ObjectFile.h"

namespace devtools_crosstool_autofdo {
class MiniDisassembler {
 public:
  // Creates a MiniDisassembler for `object_file`. Does not take ownership of
  // `object_file`, which must point to a valid object that outlives the
  // `MiniDisassembler`.
  static absl::StatusOr<absl::Nonnull<std::unique_ptr<MiniDisassembler>>>
  Create(const llvm::object::ObjectFile *object_file);

  MiniDisassembler(const MiniDisassembler &) = delete;
  MiniDisassembler(MiniDisassembler &&) = delete;

  MiniDisassembler &operator=(const MiniDisassembler &) = delete;
  MiniDisassembler &operator=(MiniDisassembler &&) = delete;

  absl::StatusOr<llvm::MCInst> DisassembleOne(uint64_t binary_address);
  bool MayAffectControlFlow(const llvm::MCInst &inst);
  llvm::StringRef GetInstructionName(const llvm::MCInst &inst) const;
  absl::StatusOr<bool> MayAffectControlFlow(uint64_t binary_address);

 private:
  explicit MiniDisassembler(const llvm::object::ObjectFile *object_file)
      : object_file_(object_file) {}

  const llvm::object::ObjectFile *object_file_;
  std::unique_ptr<const llvm::MCRegisterInfo> mri_;
  std::unique_ptr<const llvm::MCAsmInfo> asm_info_;
  std::unique_ptr<const llvm::MCSubtargetInfo> sti_;
  std::unique_ptr<const llvm::MCInstrInfo> mii_;
  std::unique_ptr<llvm::MCContext> ctx_;
  std::unique_ptr<const llvm::MCInstrAnalysis> mia_;
  std::unique_ptr<const llvm::MCDisassembler> disasm_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDOMINI_DISASSEMBLER_H_
