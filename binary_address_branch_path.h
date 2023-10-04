#ifndef AUTOFDO_BINARY_ADDRESS_BRANCH_PATH_H_
#define AUTOFDO_BINARY_ADDRESS_BRANCH_PATH_H_

#include <cstdint>
#include <vector>

#include "lbr_aggregation.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/str_join.h"
#include "third_party/abseil/absl/time/time.h"

namespace devtools_crosstool_autofdo {

struct BinaryAddressBranchPath {
  int64_t pid;
  absl::Time sample_time;
  std::vector<devtools_crosstool_autofdo::BinaryAddressBranch> branches;

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const BinaryAddressBranchPath &path) {
    absl::Format(&sink, "BinaryAddressBranchPath[pid:%lld, branches:%s]",
                 path.pid, absl::StrJoin(path.branches, ", "));
  }
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_BINARY_ADDRESS_BRANCH_PATH_H_
