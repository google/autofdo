#ifndef AUTOFDO_LLVM_PROFILE_WRITER_H_
#define AUTOFDO_LLVM_PROFILE_WRITER_H_

#if defined(HAVE_LLVM)
#include "profile_writer.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/ProfileData/SampleProfWriter.h"

namespace devtools_crosstool_autofdo {

// Writer class for LLVM profiles.
class LLVMProfileWriter : public ProfileWriter {
 public:
  explicit LLVMProfileWriter(
      llvm::sampleprof::SampleProfileFormat output_format)
      : format_(output_format) {}

  llvm::sampleprof::SampleProfileWriter *CreateSampleWriter(
      const std::string &output_filename);

  bool WriteToFile(const std::string &output_filename) override;

  llvm::sampleprof::SampleProfileWriter *GetSampleProfileWriter() {
    return sample_prof_writer_.get();
  }

 private:
  llvm::sampleprof::SampleProfileFormat format_;
  std::unique_ptr<llvm::sampleprof::SampleProfileWriter> sample_prof_writer_;

  DISALLOW_COPY_AND_ASSIGN(LLVMProfileWriter);
};

class LLVMProfileBuilder : public SymbolTraverser {
 public:
  explicit LLVMProfileBuilder(const StringIndexMap &name_table)
      : profiles_(),
        result_(llvm::sampleprof_error::success),
        inline_stack_(),
        name_table_(name_table) {}

  static bool Write(
      const std::string &output_filename,
      llvm::sampleprof::SampleProfileFormat format, const SymbolMap &symbol_map,
      const StringIndexMap &name_table,
      llvm::sampleprof::SampleProfileWriter *sample_profile_writer);

// LLVM_BEFORE_SAMPLEFDO_SPLIT_CONTEXT is defined when llvm version is before
// https://reviews.llvm.org/rGb9db70369b7799887b817e13109801795e4d70fc
#ifndef LLVM_BEFORE_SAMPLEFDO_SPLIT_CONTEXT
  const llvm::sampleprof::SampleProfileMap &ConvertProfiles(
      const SymbolMap &symbol_map);

  const llvm::sampleprof::SampleProfileMap &GetProfiles() const {
    return profiles_;
  }
#else
  const llvm::StringMap<llvm::sampleprof::FunctionSamples> &ConvertProfiles(
      const SymbolMap &symbol_map);

  const llvm::StringMap<llvm::sampleprof::FunctionSamples> &GetProfiles()
      const {
    return profiles_;
  }
#endif

 protected:
  void VisitTopSymbol(const std::string &name, const Symbol *node) override;
  void VisitCallsite(const Callsite &callsite) override;
  void Visit(const Symbol *node) override;
  llvm::StringRef GetNameRef(const std::string &str);

 private:
// LLVM_BEFORE_SAMPLEFDO_SPLIT_CONTEXT is defined when llvm version is before
// https://reviews.llvm.org/rGb9db70369b7799887b817e13109801795e4d70fc
#ifndef LLVM_BEFORE_SAMPLEFDO_SPLIT_CONTEXT
  llvm::sampleprof::SampleProfileMap profiles_;
#else
  llvm::StringMap<llvm::sampleprof::FunctionSamples> profiles_;
#endif
  llvm::sampleprof_error result_;
  std::vector<llvm::sampleprof::FunctionSamples *> inline_stack_;
  const StringIndexMap &name_table_;

  DISALLOW_COPY_AND_ASSIGN(LLVMProfileBuilder);
};
}  // namespace devtools_crosstool_autofdo

#endif  // HAVE_LLVM

#endif  // AUTOFDO_LLVM_PROFILE_WRITER_H_
