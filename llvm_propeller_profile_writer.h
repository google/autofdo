#ifndef AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_
#define AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_

#include <string>

#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_profile.h"
#include "base/logging.h"

namespace devtools_crosstool_autofdo {
// Writes the propeller profiles to output files.
class PropellerProfileWriter {
 public:
  explicit PropellerProfileWriter(const PropellerOptions& options)
      : options_(options),
        profile_encoding_(GetProfileEncoding(options.cluster_out_version())) {}

  // Writes code layout result in `all_functions_cluster_info` into the output
  // file.
  void Write(const PropellerProfile& profile) const;

 private:
  struct ProfileEncoding {
    ClusterEncodingVersion version;
    std::string version_specifier;
    std::string function_name_specifier;
    std::string function_name_separator;
    std::string module_name_specifier;
    std::string cluster_specifier;
    std::string clone_path_specifier;
  };

  static ProfileEncoding GetProfileEncoding(
      const ClusterEncodingVersion& version) {
    switch (version) {
      case ClusterEncodingVersion::VERSION_0:
        return {.version = version,
                .version_specifier = "v0",
                .function_name_specifier = "!",
                .function_name_separator = "/",
                .module_name_specifier = " M=",
                .cluster_specifier = "!!",
                .clone_path_specifier = "#NOT_SUPPORTED"};
      case ClusterEncodingVersion::VERSION_1:
        return {.version = version,
                .version_specifier = "v1",
                .function_name_specifier = "f ",
                .function_name_separator = " ",
                .module_name_specifier = "m ",
                .cluster_specifier = "c",
                .clone_path_specifier = "p"};
      default:
        LOG(FATAL) << "Unknown value for ClusterEncodingVersion: "
                   << static_cast<int>(version);
    }
  }
  PropellerOptions options_;
  ProfileEncoding profile_encoding_;
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_PROFILE_WRITER_H_
