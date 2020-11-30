#ifndef AUTOFDO_LLVM_PROPELLER_OPTIONS_H_
#define AUTOFDO_LLVM_PROPELLER_OPTIONS_H_

#include <string>

namespace devtools_crosstool_autofdo {
// This class wraps propeller options that are specified via command line
// flags.
struct PropellerOptions {
  std::string binary_name;
  std::string perf_name;
  std::string out_name;
  std::string symorder_name;
  std::string profiled_binary_name;
  bool ignore_build_id = false;

  // Internal options used by unittest to keep frontend data.
  bool keep_frontend_intermediate_data = false;

  PropellerOptions &WithBinaryName(const std::string &bn) {
    binary_name = bn;
    return *this;
  }

  PropellerOptions &WithPerfName(const std::string &pn) {
    perf_name = pn;
    return *this;
  }

  PropellerOptions &WithOutName(const std::string &on) {
    out_name = on;
    return *this;
  }

  PropellerOptions &WithSymOrderName(const std::string &so) {
    symorder_name = so;
    return *this;
  }

  PropellerOptions &WithProfiledBinaryName(const std::string &v) {
    profiled_binary_name = v;
    return *this;
  }

  PropellerOptions &WithIgnoreBuildId(bool v) {
    ignore_build_id = v;
    return *this;
  }

  PropellerOptions &WithKeepFrontendIntermediateData(bool v) {
    keep_frontend_intermediate_data = v;
    return *this;
  }
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_OPTIONS_H_
