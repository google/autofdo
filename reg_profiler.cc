#include "reg_profiler.h"

#include <optional>
#include <ostream>
#include <fstream>
#include <string>
#include <utility>

#if defined(HAVE_LLVM)
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"

namespace devtools_crosstool_autofdo {

bool PushPopCounter::Disassembler::Context::initialize(llvm::object::ObjectFile *obj) {
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86Disassembler();
  llvm::Triple triple;
  triple.setArch(llvm::Triple::ArchType(obj->getArch()));

  std::string err;
  const llvm::Target *target =
      llvm::TargetRegistry::lookupTarget(triple.normalize(), err);
  if (target == nullptr) {
    LOG(ERROR) << "Can not find the target triple.normalize(): " << err;
    return false;
  }

  mri.reset(target->createMCRegInfo(triple.getTriple()));
  if (mri == nullptr) {
    LOG(ERROR) << "Can not create MCRegInfo";
    return false;
  }

  asm_info.reset(target->createMCAsmInfo(*mri, triple.getTriple(),
                                         llvm::MCTargetOptions()));
  if (asm_info == nullptr) {
    LOG(ERROR) << "Can not create MCAsmInfo";
    return false;
  }

  sti.reset(target->createMCSubtargetInfo(triple.getTriple(), "", ""));
  if (sti == nullptr) {
    LOG(ERROR) << "Can not create MCSubtagetInfo";
    return false;
  }

  mii.reset(target->createMCInstrInfo());
  if (mii == nullptr) {
    LOG(ERROR) << "Can not create MCInstrInfo";
    return false;
  }

  ctx.reset(new llvm::MCContext(triple, asm_info.get(), mri.get(), sti.get()));
  disasm.reset(target->createMCDisassembler(*sti, *ctx));
  if (disasm == nullptr) {
    LOG(ERROR) << "Can not create MCDisassembler";
    return false;
  }

  mia.reset(target->createMCInstrAnalysis(mii.get()));
  if (mia == nullptr) {
    LOG(ERROR) << "Can not create MCInstrAnalysis";
    return false;
  }

  return true;
}

bool PushPopCounter::Disassembler::DisassembleRange(
    uint64_t start, uint64_t size, llvm::object::SectionRef &out_section,
    std::function<bool(llvm::MCInst, Disassembler::Context &)> handler) {
  for (const auto section : obj_->sections()) {
    if (!section.isText() || section.isVirtual()) {
      continue;
    }
    if (start < section.getAddress() ||
        start >= section.getAddress() + section.getSize()) {
      continue;
    }

    llvm::Expected<llvm::StringRef> bytes_str = section.getContents();
    if (!bytes_str) {
      return false;
    }

    out_section = section;
    llvm::ArrayRef<uint8_t> bytes(
        reinterpret_cast<const uint8_t *>(bytes_str->data()),
        bytes_str->size());

    uint64_t inst_len = 0;
    for (uint64_t i = start; i < start + size; i += inst_len) {
      llvm::MCInst inst;
      if (!context_.disasm->getInstruction(
              inst, inst_len, bytes.slice(i - section.getAddress()), i,
              llvm::nulls())) {
        LOG(ERROR) << "disassemble failed";
        return false;
      }
      if (handler(inst, context_)) {
        return true;
      }
    }
    return true;
  }
  return false;
}

PushPopCounter::PushAndPop PushPopCounter::Count() {
  for (auto &section : elf_obj_->sections()) {
    LOG(INFO) << section.getName()->str() << " start at "
              << section.getAddress();
  }
  bool use_filter = !filter_functions_.empty();
  for (const auto &f : info_.cfgs()) {
    if (use_filter && !not_in_list_)
      if (filter_functions_.count(f.first.str()) == 0) continue;
    if (use_filter && not_in_list_)
      if (filter_functions_.count(f.first.str()) != 0) continue;

    for (auto &node : f.second->nodes()) {
      PushAndPop in_bb;
      llvm::object::SectionRef section;
      bool success = disasm_->DisassembleRange(
          node->addr(), node->size(), section,
          [&](llvm::MCInst inst, Disassembler::Context &ctx) -> bool {
            if (ctx.mii->getName(inst.getOpcode()) == "PUSH64r") in_bb.push += 1;
            if (ctx.mii->getName(inst.getOpcode()) == "POP64r") in_bb.pop += 1;
            llvm::errs() << ctx.mii->getName(inst.getOpcode()) << "\n";
            return false;  // return true will stop disassembing
          });
      if (!success) { 
        LOG(ERROR) << "Disassemble failed";
        continue;
      }
      PushAndPop sum = (in_bb * node->freq());

      data_[all] += sum;
      if (section.getName()->startswith(".text.unlikely"))
        data_[in_unlikely] += sum;
      if (section.getName()->startswith(".text.hot"))
        data_[in_hot] += sum;
      if (section.getName()->startswith(".text.split"))
        data_[in_split] += sum;
      if (section.getName()->startswith(".text.startup"))
        data_[in_startup] += sum;
    }
  }
  return data_[all];
}

void PushPopCounter::setFilterFunctions(std::string file_path) {
  std::fstream fin(file_path, std::ios_base::in);
  std::string name;
  while (std::getline(fin, name)) {
    filter_functions_.insert(name);
  }
  if (filter_functions_.empty()) {
    LOG(ERROR) << "Filiter functions load failed: the file not exists or empty";
  }
}

}  
#endif