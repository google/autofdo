// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Main function to merge different type of profile into txt profile.

#include "base/common.h"
#include "profile_creator.h"

DEFINE_string(profile, "data.profile",
              "Profile file name");
DEFINE_string(profiler, "perf",
              "Profile type");
DEFINE_string(output_file, "data.txt",
              "Merged profile file name");
DEFINE_string(binary, "data.binary",
              "Binary file name");

int main(int argc, char **argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  if (autofdo::MergeSample(
      FLAGS_profile, FLAGS_profiler, FLAGS_binary, FLAGS_output_file)) {
    return 0;
  } else {
    return -1;
  }
}
