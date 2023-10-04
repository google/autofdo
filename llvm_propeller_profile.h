#ifndef AUTOFDO_LLVM_PROPELLER_PROFILE_H_
#define AUTOFDO_LLVM_PROPELLER_PROFILE_H_

#include <memory>
#include <vector>

#include "llvm_propeller_function_cluster_info.h"
#include "llvm_propeller_program_cfg.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {

struct PropellerProfile {
  std::unique_ptr<ProgramCfg> program_cfg;
  // Layout of functions in each section.
  absl::btree_map<llvm::StringRef, std::vector<FunctionClusterInfo>>
      functions_cluster_info_by_section_name;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_PROFILE_H_
