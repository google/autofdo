#ifndef AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_
#define AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_

#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_profile.h"

namespace devtools_crosstool_autofdo {
// Writes the propeller profiles to output files.
class PropellerProfileWriter {
 public:
  explicit PropellerProfileWriter(const PropellerOptions& options)
      : options_(options) {}

  // Writes code layout result in `all_functions_cluster_info` into the output
  // file.
  void Write(const PropellerProfile& profile) const;

 private:
  PropellerOptions options_;
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_
