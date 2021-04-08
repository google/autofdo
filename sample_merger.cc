// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Main function to merge different type of profile into txt profile.

#include "base/commandlineflags.h"
#include "profile_creator.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"

ABSL_FLAG(string, profile, "data.profile", "Profile file name");
ABSL_FLAG(string, profiler, "perf", "Profile type");
ABSL_FLAG(string, output_file, "data.txt", "Merged profile file name");
ABSL_FLAG(string, binary, "data.binary", "Binary file name");

int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(argv[0]);
  absl::ParseCommandLine(argc, argv);

  if (devtools_crosstool_autofdo::MergeSample(
          absl::GetFlag(FLAGS_profile), absl::GetFlag(FLAGS_profiler),
          absl::GetFlag(FLAGS_binary), absl::GetFlag(FLAGS_output_file))) {
    return 0;
  } else {
    return -1;
  }
}
