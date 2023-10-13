#ifndef AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_
#define AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_

#if defined(HAVE_LLVM)

#include <memory>
#include <utility>
#include <vector>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_statistics.h"
#include "status_provider.h"
#include "third_party/abseil/absl/status/status.h"

namespace devtools_crosstool_autofdo {

// Propeller interface for SWIG as well as create_llvm_prof.
absl::Status GeneratePropellerProfiles(
    const devtools_crosstool_autofdo::PropellerOptions &opts);
// Like above, but `opts.perf_names` is ignored and `perf_data_provider` is used
// instead.
absl::Status GeneratePropellerProfiles(
    const devtools_crosstool_autofdo::PropellerOptions &opts,
    std::unique_ptr<PerfDataProvider> perf_data_provider);

class PropellerProfWriter {
 public:
  static std::unique_ptr<PropellerProfWriter> Create(
      const PropellerOptions &options,
      MultiStatusProvider *status = nullptr);
  // Like above, but `opts.perf_names` is ignored and `perf_data_provider` is
  // used instead.
  static std::unique_ptr<PropellerProfWriter> Create(
      const PropellerOptions &options,
      std::unique_ptr<PerfDataProvider> perf_data_provider,
      MultiStatusProvider *status = nullptr);

  // Main entrance of propeller profile writer.
  // Return true if succeeded.
  bool Write(
      const std::vector<FunctionClusterInfo> &all_functions_cluster_info);

  const PropellerStats &stats() const { return stats_; }
  void PrintStats() const;

  const AbstractPropellerWholeProgramInfo *whole_program_info() const {
    return whole_program_info_.get();
  }

  ~PropellerProfWriter() {}

 private:
  PropellerProfWriter(const PropellerOptions &options,
                      std::unique_ptr<AbstractPropellerWholeProgramInfo> wpi)
      : options_(options), whole_program_info_(std::move(wpi)) {}

  const PropellerOptions options_;
  PropellerStats stats_;
  std::unique_ptr<AbstractPropellerWholeProgramInfo> whole_program_info_;
};

}  // namespace devtools_crosstool_autofdo
#endif

#endif  // AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_
