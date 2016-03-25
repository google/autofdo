#ifndef DEVTOOLS_CROSSTOOL_AUTOFDO_LLVM_PROFILE_WRITER_H_
#define DEVTOOLS_CROSSTOOL_AUTOFDO_LLVM_PROFILE_WRITER_H_

#include "config.h"

#if defined(HAVE_LLVM)
#include "profile_writer.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/ProfileData/SampleProfWriter.h"

namespace autofdo {

// Writer class for LLVM profiles.
class LLVMProfileWriter : public ProfileWriter {
 public:
  explicit LLVMProfileWriter(
      llvm::sampleprof::SampleProfileFormat output_format)
      : format_(output_format) {}

  bool WriteToFile(const string &output_filename) override;

 private:
  llvm::sampleprof::SampleProfileFormat format_;

  DISALLOW_COPY_AND_ASSIGN(LLVMProfileWriter);
};

class LLVMProfileBuilder : public SymbolTraverser {
 public:
  explicit LLVMProfileBuilder(const StringIndexMap &name_table)
      : profiles_(),
        result_(llvm::sampleprof_error::success),
        inline_stack_(),
        name_table_(name_table) {}

  static bool Write(const string &output_filename,
                    llvm::sampleprof::SampleProfileFormat format,
                    const SymbolMap &symbol_map,
                    const StringIndexMap &name_table);

  const llvm::StringMap<llvm::sampleprof::FunctionSamples> &ConvertProfiles(
      const SymbolMap &symbol_map);

  const llvm::StringMap<llvm::sampleprof::FunctionSamples> &GetProfiles()
      const {
    return profiles_;
  }

 protected:
  void VisitTopSymbol(const string &name, const Symbol *node) override;
  void VisitCallsite(const Callsite &callsite) override;
  void Visit(const Symbol *node) override;
  llvm::StringRef GetNameRef(const string &str);

 private:
  llvm::StringMap<llvm::sampleprof::FunctionSamples> profiles_;
  llvm::sampleprof_error result_;
  std::vector<llvm::sampleprof::FunctionSamples *> inline_stack_;
  const StringIndexMap &name_table_;

  DISALLOW_COPY_AND_ASSIGN(LLVMProfileBuilder);
};
}  // namespace devtools_crosstool_autofdo

#endif  // HAVE_LLVM

#endif  // DEVTOOLS_CROSSTOOL_AUTOFDO_LLVM_PROFILE_WRITER_H_
