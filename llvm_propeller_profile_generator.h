#ifndef AUTOFDO_LLVM_PROPELLER_PROFILE_GENERATOR_H_
#define AUTOFDO_LLVM_PROPELLER_PROFILE_GENERATOR_H_

#if defined(HAVE_LLVM)
#include <memory>

#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "third_party/abseil/absl/status/status.h"

namespace devtools_crosstool_autofdo {
// Propeller interface for SWIG as well as create_llvm_prof.
absl::Status GeneratePropellerProfiles(
    const devtools_crosstool_autofdo::PropellerOptions &opts);

// Like above, but `opts.perf_names` is ignored and `perf_data_provider` is used
// instead.
absl::Status GeneratePropellerProfiles(
    const PropellerOptions &opts,
    std::unique_ptr<PerfDataProvider> perf_data_provider);
}  // namespace devtools_crosstool_autofdo
#endif

#endif  // AUTOFDO_LLVM_PROPELLER_PROFILE_GENERATOR_H_
