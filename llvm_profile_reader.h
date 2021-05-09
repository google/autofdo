// Read symbol_map from the llvm sample profile.

#ifndef AUTOFDO_LLVM_PROFILE_READER_H_
#define AUTOFDO_LLVM_PROFILE_READER_H_

#include "base_profile_reader.h"
#include "source_info.h"
#include "third_party/abseil/absl/container/node_hash_set.h"
#include "llvm/ProfileData/SampleProf.h"

namespace llvm {
class StringRef;
namespace sampleprof {
class FunctionSamples;
}
}  // namespace llvm

namespace devtools_crosstool_autofdo {
class SymbolMap;

struct SpecialSyms {
  SpecialSyms(const char *strip_all_p[], unsigned strip_all_len,
              const char *keep_sole_p[], unsigned keep_sole_len,
              const char *keep_cold_p[], unsigned keep_cold_len) {
    for (int i = 0; i < strip_all_len; i++) strip_all.insert(strip_all_p[i]);
    for (int i = 0; i < keep_sole_len; i++) keep_sole.insert(keep_sole_p[i]);
    for (int i = 0; i < keep_cold_len; i++) keep_cold.insert(keep_cold_p[i]);
  }

  absl::node_hash_set<std::string> strip_all;
  absl::node_hash_set<std::string> keep_sole;
  absl::node_hash_set<std::string> keep_cold;
  absl::node_hash_set<std::string> skip_set;
};

class LLVMProfileReader : public ProfileReader {
 public:
  explicit LLVMProfileReader(SymbolMap *symbol_map,
                             absl::node_hash_set<std::string>& names,
                             SpecialSyms *special_syms = nullptr)
      : symbol_map_(symbol_map), names_(names), special_syms_(special_syms) {}

  bool ReadFromFile(const std::string &output_file) override;

  bool shouldMergeProfileForSym(const std::string name);

  void SetProfileSymbolList(
      std::unique_ptr<llvm::sampleprof::ProfileSymbolList> list) {
    prof_sym_list_ = std::move(list);
  }

  llvm::sampleprof::ProfileSymbolList *GetProfileSymbolList() {
    return prof_sym_list_.get();
  }

 private:
  const char *GetName(const llvm::StringRef &N);

  void ReadFromFunctionSamples(const SourceStack &stack,
                               const llvm::sampleprof::FunctionSamples &fs);

  SymbolMap *symbol_map_;
  absl::node_hash_set<std::string>& names_;
  SpecialSyms *special_syms_;
  std::unique_ptr<llvm::sampleprof::ProfileSymbolList> prof_sym_list_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROFILE_READER_H_
