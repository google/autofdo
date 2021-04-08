// Copyright 2013 Google Inc. All Rights Reserved.
// Author: dnovillo@google.com (Diego Novillo)

// This program creates an LLVM profile from an AutoFDO source.

#include "third_party/abseil/absl/flags/flag.h"
#if defined(HAVE_LLVM)
#include <memory>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "llvm_profile_writer.h"
#include "llvm_propeller_code_layout.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_profile_writer.h"
#include "profile_creator.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_split.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"

ABSL_FLAG(string, profile, "perf.data",
          "Input profile file name. When --format=propeller, this accepts "
          "multiple profile file names concatnated by ';'");
ABSL_FLAG(string, profiler, "perf",
          "Input profile type. Possible values: perf, text, or prefetch");
ABSL_FLAG(string, prefetch_hints, "", "Input cache prefetch hints");
ABSL_FLAG(string, out, "", "Output profile file name");
ABSL_FLAG(string, gcov, "",
          "Output profile file name. Alias for --out; used for "
          "flag compatibility with create_gcov");
ABSL_FLAG(string, binary, "a.out", "Binary file name");
// FIXME(dnovillo) - This should default to 'binary'.  However, the binary
// representation is currently version locked to the latest LLVM upstream
// sources. This may cause incompatibilities with the currently released version
// of Crosstool LLVM.  Default to 'binary' once http://b/27336904 is fixed.
ABSL_FLAG(string, format, "text",
          "LLVM profile format to emit. Possible values are 'text', "
          "'binary', 'extbinary' or 'propeller'. The binary format is a "
          "more compact representation, but the text format is human "
          "readable and more likely to be compatible with older versions "
          "of LLVM. extbinary format is also a binary format but more "
          "easily to be extended. propeller format is used exclusively by "
          "post linker optimizer.");
ABSL_FLAG(string, propeller_symorder, "",
          "Propeller symbol ordering output file name.");
ABSL_FLAG(
    string, profiled_binary_name, "",
    "Name specified to compare against perf mmap_events. This value is usually "
    "different from \"--binary=\" option. Each perf mmap event contains an "
    "executable name, use this option to select the mmap events we are "
    "interested in. Also note, \"--profiled_binary_name\" should have the same "
    "bits as the file given by \"--binary\" but may have different name with "
    "it.");
ABSL_FLAG(bool, prof_sym_list, false,
          "Generate profile symbol list from the binary. The symbol list will "
          "be kept and saved in the profile. The option can only be enabled "
          "when --format=extbinary.");

// While reading perfdata file, we use build id to match a binary and its pids
// in perf file. We may also want to use file name to do the match, which is
// less accurate. A typical scenario to set "--ignore_build_id" is when the
// origin binary that generates the perfdata is no longer available, we thus
// have to rebuild the binary from source.
ABSL_FLAG(bool, ignore_build_id, false,
          "Ignore build id, use file name to match data in perfdata file.");

devtools_crosstool_autofdo::PropellerOptions PropellerOptionsFromFlags() {
  std::vector<std::string> perf_files =
      absl::StrSplit(absl::GetFlag(FLAGS_profile), ';');
  devtools_crosstool_autofdo::PropellerOptionsBuilder option_builder;
  for (const std::string &pf : perf_files)
    if (!pf.empty()) option_builder.AddPerfNames(pf);
  return devtools_crosstool_autofdo::PropellerOptions(
      option_builder.SetBinaryName(absl::GetFlag(FLAGS_binary))
          .SetClusterOutName(absl::GetFlag(FLAGS_out))
          .SetSymbolOrderOutName(absl::GetFlag(FLAGS_propeller_symorder))
          .SetProfiledBinaryName(absl::GetFlag(FLAGS_profiled_binary_name))
          .SetIgnoreBuildId(absl::GetFlag(FLAGS_ignore_build_id)));
}

int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(argv[0]);
  absl::ParseCommandLine(argc, argv);

  // If the user specified --gcov instead of --out, use that value.
  // If both are used, they must match.
  if (!absl::GetFlag(FLAGS_gcov).empty()) {
    if (!absl::GetFlag(FLAGS_out).empty() &&
        absl::GetFlag(FLAGS_out) != absl::GetFlag(FLAGS_gcov)) {
      LOG(ERROR) << "--out and --gcov specified with different values.";
      LOG(ERROR) << "Please use only one of the two flags.";
      return 1;
    }
    absl::SetFlag(&FLAGS_out, absl::GetFlag(FLAGS_gcov));
  }

  if (absl::GetFlag(FLAGS_out).empty()) {
    LOG(ERROR) << "Need a name for the generated LLVM profile file.";
    LOG(ERROR) << "Use --gcov or --out to specify an output file.";
    return 1;
  }

  if (absl::GetFlag(FLAGS_ignore_build_id) &&
      absl::GetFlag(FLAGS_format) != "propeller") {
    LOG(ERROR)
        << "\"--ignore_build_id\" is only valid when \"--format=propeller\".";
    return 1;
  }

  // Propeller profile format does not use CreateProfile so check it separately
  // before checking for other formats.
  if (absl::GetFlag(FLAGS_format) == "propeller") {
    absl::Status status = devtools_crosstool_autofdo::GeneratePropellerProfiles(
        PropellerOptionsFromFlags());
    if (!status.ok()) {
      LOG(ERROR) << status;
      return 1;
    }
    return 0;
  }

  std::unique_ptr<devtools_crosstool_autofdo::LLVMProfileWriter> writer(
      nullptr);
  if (absl::GetFlag(FLAGS_format) == "text") {
    writer = absl::make_unique<devtools_crosstool_autofdo::LLVMProfileWriter>(
        llvm::sampleprof::SPF_Text);
  } else if (absl::GetFlag(FLAGS_format) == "binary") {
    writer = absl::make_unique<devtools_crosstool_autofdo::LLVMProfileWriter>(
        llvm::sampleprof::SPF_Binary);
  } else if (absl::GetFlag(FLAGS_format) == "extbinary") {
    writer = absl::make_unique<devtools_crosstool_autofdo::LLVMProfileWriter>(
        llvm::sampleprof::SPF_Ext_Binary);
  } else {
    LOG(ERROR)
        << "--format=" << absl::GetFlag(FLAGS_format) << " is not supported. "
        << "Use one of 'text', 'binary', 'propeller' or 'extbinary' format";
    return 1;
  }

  if (absl::GetFlag(FLAGS_prof_sym_list) &&
      absl::GetFlag(FLAGS_format) != "extbinary") {
    LOG(ERROR) << "--prof_sym_list is enabled, --format must be extbinary";
    return 1;
  }

  devtools_crosstool_autofdo::ProfileCreator creator(
      absl::GetFlag(FLAGS_binary));
  creator.set_use_discriminator_encoding(true);
  if (creator.CreateProfile(absl::GetFlag(FLAGS_profile),
                            absl::GetFlag(FLAGS_profiler), writer.get(),
                            absl::GetFlag(FLAGS_out),
                            absl::GetFlag(FLAGS_prof_sym_list))) {
    return 0;
  } else {
    return -1;
  }
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
#endif  // HAVE_LLVM
