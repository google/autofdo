// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Main function to create AutoFDO Profile.
//
// To test if this tool is working properly with perflab:
//   1. build create_gcov: blaze build devtools/crosstoo/autofdo:create_gcov
//   2. perflab checkout k8
//   3. perflab -k --fdo=gen-afdo build k8
//   4. perflab qfetch `perflab -k --arch=ikaria_westmere \
//                      --fdo=gen-afdo qsub k8 | awk '{print $3}'`
//   5. perflab -k --arch=ikaria_westmere --create_gcov=your_create_gcov_path \
//              --fdo_profile_root=. --fdo_profile_name=name create-autoprof k8
//   6. perflab -k --fdo_profile_root=. --fdo_profile_name=name --fdo=use-afdo \
//              build k8
//   7. perflab qfetch `perflab -k --label=opt --arch=ikaria_westmere qsub k8 \
//                      | awk '{print $3}'`
//   8. perflab --arch=ikaria_westmere --label=opt table k8

#include "base/commandlineflags.h"
#include "gcov.h"
#include "profile_creator.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"

ABSL_FLAG(std::string, profile, "perf.data",
              "Profile file name");
ABSL_FLAG(std::string, profiler, "perf",
              "Profile type");
ABSL_FLAG(std::string, gcov, "fbdata.afdo",
              "Output file name");
ABSL_FLAG(std::string, binary, "data.binary",
              "Binary file name");

int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(argv[0]);
  absl::ParseCommandLine(argc, argv);

  devtools_crosstool_autofdo::AutoFDOProfileWriter writer(
      absl::GetFlag(FLAGS_gcov_version));
  devtools_crosstool_autofdo::ProfileCreator creator(
      absl::GetFlag(FLAGS_binary));
  if (creator.CreateProfile(absl::GetFlag(FLAGS_profile),
                            absl::GetFlag(FLAGS_profiler), &writer,
                            absl::GetFlag(FLAGS_gcov))) {
    return 0;
  } else {
    return -1;
  }
}
