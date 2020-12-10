#include "llvm_propeller_protobuf.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"
#if defined(HAVE_LLVM)

#include <memory>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "llvm_propeller_options.h"
#include "llvm_propeller_whole_program_info.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"

DEFINE_string(binary, "a.out", "Binary file name");
DEFINE_string(profile, "perf.data", "Input profile file name");
DEFINE_string(propeller_protobuf_out, "",
              "Instruct propeller to output a protobuf file which can reused "
              "as testdata input for the code layout algorithm.");
DEFINE_string(profiled_binary_name, "",
              "Name specified to compare against perf mmap_events. Same as the "
              "option in create_llvm_prof.");
DEFINE_bool(ignore_build_id, false,
            "Ignore build id, use file name to match data in perfdata file. "
            "Same as the option in create_llvm_prof.");

using ::devtools_crosstool_autofdo::PropellerOptions;
using ::devtools_crosstool_autofdo::PropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::PropProt;

// This binary is used to generate text protobuf format files for use as
// testdata for codelayout tests. Example usage:
// $ blaze run -c opt :llvm_propeller_generate_testdata -- \
// --binary=`pwd`/testdata/propeller_sample.bin \
// --profiled_binary_name="propeller_sample.bin" \
// --profile=`pwd`/testdata/propeller_sample.perfdata \
// --propeller_protobuf_out=$HOME/tmp/propeller_sample.pb
int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(argv[0]);
  absl::ParseCommandLine(argc, argv);

  if (absl::GetFlag(FLAGS_propeller_protobuf_out).empty()) {
    LOG(ERROR) << "Path to output protobuf must be specified.";
    return 1;
  }

  PropellerOptions options{
      .binary_name = FLAGS_binary,
      .perf_name = FLAGS_profile,
      .profiled_binary_name = FLAGS_profiled_binary_name,
      .ignore_build_id = absl::GetFlag(FLAGS_ignore_build_id),
      .keep_frontend_intermediate_data = true};

  std::unique_ptr<PropellerWholeProgramInfo> whole_program_info =
      PropellerWholeProgramInfo::Create(options);

  if (!whole_program_info->CreateCfgs()) {
    LOG(ERROR) << "Could not create cfs for whole program.";
    return 1;
  }

  PropProt prop_prot;
  if (!prop_prot.InitWriter(FLAGS_propeller_protobuf_out)) {
    LOG(ERROR) << "Could not initialize protobuf writer.";
    return 1;
  }
  prop_prot.AddCfgs(whole_program_info->cfgs());
  if (!prop_prot.WriteAndClose()) {
    LOG(ERROR) << "Could not write testdata protobuf file.";
    return 1;
  }
  return 0;
}

#else
#include <stdio.h>
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"
int main(int argc, char **argv) {
  fprintf(stderr,
          "ERROR: LLVM support was not enabled in this configuration.\nPlease "
          "configure and rebuild with:\n\n$ ./configure "
          "--with-llvm=<path-to-llvm-config>\n\n");
  return -1;
}
#endif  // HAVE_LLVM }
