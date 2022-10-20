#ifndef EXPERIMENTAL_USERS_XIAOFANS_REG_PROFILER_REG_PROFILER_H_
#define EXPERIMENTAL_USERS_XIAOFANS_REG_PROFILER_REG_PROFILER_H_

#if defined(HAVE_LLVM)

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <set>

#include "llvm_propeller_whole_program_info.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"

namespace devtools_crosstool_autofdo {



class PushPopCounter {
 public:
  struct PushAndPop {
    uint64_t push = 0, pop = 0;
    PushAndPop operator+(PushAndPop other) const {
      return {push + other.push, pop + other.pop};
    }
    PushAndPop operator+=(PushAndPop other) {
      push += other.push;
      pop += other.pop;
      return *this;
    }
    PushAndPop operator*(int64_t q) const { return {push * q, pop * q}; }
  };

  class Disassembler {
   public:
    struct Context {
      bool initialize(llvm::object::ObjectFile *obj);

      std::unique_ptr<const llvm::MCRegisterInfo> mri;
      std::unique_ptr<const llvm::MCAsmInfo> asm_info;
      std::unique_ptr<const llvm::MCSubtargetInfo> sti;
      std::unique_ptr<const llvm::MCInstrInfo> mii;
      std::unique_ptr<llvm::MCContext> ctx;
      std::unique_ptr<const llvm::MCDisassembler> disasm;
      std::unique_ptr<const llvm::MCInstrAnalysis> mia;
    };

    explicit Disassembler(llvm::object::ObjectFile *obj) : obj_(obj) {
      context_.initialize(obj);
    }
    ~Disassembler() {}

    bool DisassembleRange(
        uint64_t start, uint64_t size, llvm::object::SectionRef &out_section,
        std::function<bool(llvm::MCInst, Disassembler::Context &)> handler);

   private:
    llvm::object::ObjectFile *obj_;
    Context context_;
  };

  explicit PushPopCounter(
      const devtools_crosstool_autofdo::PropellerWholeProgramInfo &info)
      : info_(info),
        disasm_(new Disassembler(info.binray_perf_info().binary_info.object_file.get())) {
    elf_obj_ = llvm::dyn_cast<llvm::object::ELFObjectFileBase,
                              llvm::object::ObjectFile>(
        info.binray_perf_info().binary_info.object_file.get());
  }

  enum count_type { all = 0, in_unlikely, in_hot, in_split, in_startup };

  PushAndPop Count();

  PushAndPop *getData() { return data_; }

  void setFilterFunctions(std::string file_path);
  void setNotInList(bool not_in_list) { not_in_list_ = not_in_list; }

 private:
  const devtools_crosstool_autofdo::PropellerWholeProgramInfo &info_;
  llvm::object::ELFObjectFileBase *elf_obj_;
  std::unique_ptr<Disassembler> disasm_;

  PushAndPop data_[5];
  std::set<std::string> filter_functions_;
  bool not_in_list_ = false;  // find register usage of functions
                              // which is not in filter_functions_
};

}  

#endif

#endif  // EXPERIMENTAL_USERS_XIAOFANS_REG_PROFILER_REG_PROFILER_H_
