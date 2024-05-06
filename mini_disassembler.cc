#include "mini_disassembler.h"

#include <cstdint>
#include <memory>
#include <string>

#include "base/logging.h"
#include "third_party/abseil/absl/base/nullability.h"
#include "third_party/abseil/absl/memory/memory.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

namespace devtools_crosstool_autofdo {
absl::StatusOr<absl::Nonnull<std::unique_ptr<MiniDisassembler>>>
MiniDisassembler::Create(const llvm::object::ObjectFile *object_file) {
  auto disassembler =
      absl::WrapUnique<MiniDisassembler>(new MiniDisassembler(object_file));

  std::string err;
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllDisassemblers();

  llvm::Triple triple;
  triple.setArch(
      llvm::Triple::ArchType(object_file->getArch()));
  const llvm::Target *target =
      llvm::TargetRegistry::lookupTarget(triple.normalize(), err);
  if (target == nullptr) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "no target for triple '%s': %s", triple.getArchName(), err));
  }
  disassembler->mri_ =
      absl::WrapUnique(target->createMCRegInfo(triple.getTriple()));
  if (disassembler->mri_ == nullptr) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "createMCRegInfo failed for triple '%s'", triple.getArchName()));
  }
  disassembler->asm_info_ = absl::WrapUnique(target->createMCAsmInfo(
      *disassembler->mri_, triple.getTriple(), llvm::MCTargetOptions()));
  if (disassembler->asm_info_ == nullptr) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "createMCAsmInfo failed for triple '%s'", triple.getArchName()));
  }

  disassembler->sti_ = absl::WrapUnique(target->createMCSubtargetInfo(
      triple.getTriple(), /*CPU=*/"", /*Features=*/""));
  if (disassembler->sti_ == nullptr) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "createMCSubtargetInfo failed for triple '%s'", triple.getArchName()));
  }

  disassembler->mii_ = absl::WrapUnique(target->createMCInstrInfo());
  if (disassembler->mii_ == nullptr) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "createMCInstrInfo failed for triple '%s'", triple.getArchName()));
  }

  disassembler->mia_ =
      absl::WrapUnique(target->createMCInstrAnalysis(disassembler->mii_.get()));
  if (disassembler->mia_ == nullptr) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "createMCInstrAnalysis failed for triple '%s'", triple.getArchName()));
  }

  disassembler->ctx_ = std::make_unique<llvm::MCContext>(
      triple, disassembler->asm_info_.get(), disassembler->mri_.get(),
      disassembler->sti_.get());
  disassembler->disasm_ = absl::WrapUnique(
      target->createMCDisassembler(*disassembler->sti_, *disassembler->ctx_));
  if (disassembler->disasm_ == nullptr)
    return absl::FailedPreconditionError(
        absl::StrFormat("createMCDisassembler failed"));

  return disassembler;
}

absl::StatusOr<llvm::MCInst> MiniDisassembler::DisassembleOne(
    uint64_t binary_address) {
  for (const auto &section : object_file_->sections()) {
    if (!section.isText() || section.isVirtual()) {
      continue;
    }
    if (binary_address < section.getAddress() ||
        binary_address >= section.getAddress() + section.getSize()) {
      continue;
    }
    llvm::Expected<llvm::StringRef> content = section.getContents();
    if (!content) {
      return absl::FailedPreconditionError("section has no content");
    }
    llvm::ArrayRef<uint8_t> content_bytes(
        reinterpret_cast<const uint8_t *>(content->data()), content->size());
    uint64_t section_offset = binary_address - section.getAddress();
    llvm::MCInst inst;
    uint64_t size;
    if (!disasm_->getInstruction(inst, size,
                                 content_bytes.slice(section_offset),
                                 binary_address, llvm::nulls())) {
      return absl::FailedPreconditionError(absl::StrFormat(
          "getInstruction failed at binary address 0x%lx", binary_address));
    }
    return inst;
  }
  return absl::FailedPreconditionError(absl::StrFormat(
      "no section containing address 0x%lx found", binary_address));
}

bool MiniDisassembler::MayAffectControlFlow(const llvm::MCInst &inst) {
  return mii_->get(inst.getOpcode()).mayAffectControlFlow(inst, *mri_);
}

llvm::StringRef MiniDisassembler::GetInstructionName(
    const llvm::MCInst &inst) const {
  return mii_->getName(inst.getOpcode());
}

absl::StatusOr<bool> MiniDisassembler::MayAffectControlFlow(
    uint64_t binary_address) {
  auto inst = DisassembleOne(binary_address);
  if (!inst.ok()) return inst.status();
  return MayAffectControlFlow(inst.value());
}
}  // namespace devtools_crosstool_autofdo
