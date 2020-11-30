// utility to process profile symbol list.
#include "profile_symbol_list.h"

#include <algorithm>
#include <functional>
#include <numeric>
#include <vector>

#include "symbol_map.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "llvm/ProfileData/SampleProf.h"

namespace devtools_crosstool_autofdo {

namespace {
bool IsValid(double ratio) { return ratio >= 0.0 && ratio <= 1.0; }
}  // namespace

// Populate prof_sym_list with the symbols whose sizes contribute to k percent
// of the total sizes in name_size_list (k=100*symbol_list_size_coverage_ratio).
// E.g., when symbol_list_size_coverage_ratio is 0.0, no function symbol is
// listed; when symbol_list_size_coverage_ratio is 1.0, all function symbols are
// listed.
//
// Precondition: one of `symbol_list_size_coverage_ratio` and the
// FLAGS_symbol_list_size_coverage_ratio must be -1.0, the other one must be a
// valid value in range [0.0, 1.0].
void fillProfileSymbolList(llvm::sampleprof::ProfileSymbolList *prof_sym_list,
                           const NameSizeList &name_size_list,
                           const SymbolMap *symbol_map,
                           double symbol_list_size_coverage_ratio) {
  if (name_size_list.empty()) {
    return;
  }

  if (!IsValid(symbol_list_size_coverage_ratio)) {
    LOG(FATAL) << "Parameter symbol_list_size_coverage_ratio="
               << symbol_list_size_coverage_ratio
               << "; it must be in range [0.0, 1.0].";
  }

  if (symbol_list_size_coverage_ratio == 0) {
    return;
  }

  std::vector<uint64_t> sizes;
  uint64_t total_size = 0;
  for (const auto &symbol_entry : name_size_list) {
    uint64_t symbol_size = symbol_entry.second;
    sizes.push_back(symbol_size);
    total_size += symbol_size;
  }
  std::sort(sizes.begin(), sizes.end(), std::greater<>());
  uint64_t total_threshold = total_size * symbol_list_size_coverage_ratio;
  if (sizes[0] > total_threshold) {
    // This means that the biggest size already contributes
    // more than the threshold, so we won't keep any symbol.
    return;
  }

  std::vector<uint64_t> psum(sizes.size());
  std::partial_sum(sizes.begin(), sizes.end(), psum.begin());
  auto lower = std::lower_bound(psum.begin(), psum.end(), total_threshold,
                                std::less<>());
  uint64_t cutoff = sizes[std::distance(psum.begin(), lower)];

  // Add symbol names with size larger than or equal to the cutoff into
  // prof_sym_list.
  for (const auto &symbol_entry : name_size_list) {
    if (symbol_entry.second >= cutoff) {
      const llvm::StringRef &name = symbol_entry.first;
      std::string original_name = symbol_map->GetOriginalName(name.data());
      if (original_name == name) {
        prof_sym_list->add(name);
        continue;
      }
      prof_sym_list->add(original_name, true);
    }
  }
}

}  // namespace devtools_crosstool_autofdo
