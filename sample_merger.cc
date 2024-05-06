// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Main function to merge different type of profile into txt profile.

#include <string>
#include <vector>

#include "base/commandlineflags.h"
#include "profile_creator.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"

ABSL_FLAG(std::string, profile, "data.profile",
          "Input profile filename. Use this flag if you only have a single "
          "profile to merge. Otherwise prefer --profiles flag");
ABSL_FLAG(
    std::vector<std::string>, profiles, {},
    "Input profile filenames. All files are merged into the --output_file");
ABSL_FLAG(std::string, profiler, "perf", "Profile type");
ABSL_FLAG(std::string, output_file, "data.txt", "Merged profile file name");
ABSL_FLAG(std::string, binary, "data.binary", "Binary file name");

int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(argv[0]);
  absl::ParseCommandLine(argc, argv);

  std::vector<std::string> profiles = absl::GetFlag(FLAGS_profiles);
  if (profiles.empty()) {
    profiles.push_back(absl::GetFlag(FLAGS_profile));
  }
  if (devtools_crosstool_autofdo::MergeSample(
          profiles, absl::GetFlag(FLAGS_profiler), absl::GetFlag(FLAGS_binary),
          absl::GetFlag(FLAGS_output_file))) {
    return 0;
  } else {
    return 1;
  }
}
