// utility to process profile symbol list.
#ifndef AUTOFDO_PROFILE_SYMBOL_LIST_H_
#define AUTOFDO_PROFILE_SYMBOL_LIST_H_

#include <utility>
#include <vector>

#include "symbol_map.h"
#include "llvm/ProfileData/SampleProf.h"

// Common flags used by most autofdo tools.
#define AUTOFDO_PROFILE_SYMBOL_LIST_FLAGS                                  \
  ABSL_FLAG(                                                               \
      double, symbol_list_size_coverage_ratio, 1.0,                        \
      "List the largest function symbols where their sizes contribute to " \
      "k percent of the total sizes "                                      \
      "(k=100*symbol_list_size_coverage_ratio). E.g., when "               \
      "this is 0.0, no function symbol is listed; when this is 1.0, all "  \
      "function symbols are listed."                                       \
      "Please use the flag with caution if your tool could allow "         \
      "different values for different targets -- the flag will override "  \
      "them with a single value. ");                                       \
  ABSL_FLAG(bool, compress_symbol_list, true,                              \
            "whether to compress the symbol list.");

namespace devtools_crosstool_autofdo {

// Vector of pairs containing symbol name and size.
using SymbolList = std::vector<std::pair<llvm::StringRef, uint64>>;

void fillProfileSymbolList(llvm::sampleprof::ProfileSymbolList *prof_sym_list,
                           const NameSizeList &name_size_list,
                           const SymbolMap *symbol_map,
                           double symbol_list_size_coverage_ratio = -1.0);

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_PROFILE_SYMBOL_LIST_H_
