#ifndef AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_
#define AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_

#if defined(HAVE_LLVM)

#include <memory>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/status/status.h"

namespace devtools_crosstool_autofdo {

// Propeller interface for SWIG as well as create_llvm_prof. Arguments:
//  `binary_file_name`: binary file name
//  `perf_file_name`: perf.data file name
//  `cluster_output_file_name`: propeller cluster information output file name
//  `order_output_file_name`: propeller symbol order output file name
//  `profiled_binary_name`: if not empty, use this as file name to match MMAP
//                        events in perf.data file, can be "".
//  `ignore_build_id`: if set, use binary_file_name or profiled_binary_name to
//                     match MMAP events. Otherwise, use buildid (if exists)
//                     to match.
absl::Status GeneratePropellerProfiles(
    const std::string &binary_file_name, const std::string &perf_file_name,
    const std::string &cluster_output_file_name,
    const std::string &order_output_file_name,
    const std::string &profiled_binary_name, bool ignore_build_id);

class PropellerProfWriter {
 public:
  static std::unique_ptr<PropellerProfWriter> Create(
      const PropellerOptions &options);

  // Main entrance of propeller profile writer.
  // Return true if succeeded.
  bool Write(const CodeLayoutResult &layout_cluster_info);

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

