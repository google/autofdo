#ifndef AUTOFDO_LLVM_PROPELLER_PROFILE_GENERATOR_H_
#define AUTOFDO_LLVM_PROPELLER_PROFILE_GENERATOR_H_

#include <optional>

#include "llvm_propeller_perf_data_provider.h"
#if defined(HAVE_LLVM)
#include <memory>

#include "llvm_propeller_options.pb.h"
#include "third_party/abseil/absl/status/status.h"

namespace devtools_crosstool_autofdo {
// Propeller interface for SWIG as well as create_llvm_prof.
absl::Status GeneratePropellerProfiles(
    const devtools_crosstool_autofdo::PropellerOptions &opts);

// Like above, but `opts.profiles` is ignored and `perf_data_provider` is
// used instead, and the perf data it yields is interpreted as `profile_type`.
// Returns an error if `profile_type` is not Perf LBR or SPE.
absl::Status GeneratePropellerProfiles(
    const PropellerOptions &opts,
    std::unique_ptr<PerfDataProvider> perf_data_provider,
    ProfileType profile_type = ProfileType::PERF_LBR);
}  // namespace devtools_crosstool_autofdo
#endif

#endif  // AUTOFDO_LLVM_PROPELLER_PROFILE_GENERATOR_H_
