// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This program creates an LLVM profile from an AutoFDO source.

#include "gflags/gflags.h"
#include "profile_creator.h"

DEFINE_string(profile, "perf.data", "Input profile file name");
DEFINE_string(profiler, "perf", "Input profile type");
DEFINE_string(out, "", "Output profile file name");
DEFINE_string(gcov, "",
              "Output profile file name. Alias for --out; used for "
              "flag compatibility with create_gcov");
DEFINE_string(binary, "a.out", "Binary file name");

#define PROG_USAGE                                           \
  "\nConverts a sample profile collected with Perf "         \
  "(https://perf.wiki.kernel.org/)\n"                        \
  "into an LLVM profile. The output file can be used with\n" \
  "Clang's -fprofile-sample-use flag."

int main(int argc, char **argv) {
  google::SetUsageMessage(PROG_USAGE);
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  // If the user specified --gcov instead of --out, use that value.
  // If both are used, they must match.
  if (!FLAGS_gcov.empty()) {
    if (!FLAGS_out.empty() && FLAGS_out != FLAGS_gcov) {
      LOG(ERROR) << "--out and --gcov specified with different values.";
      LOG(ERROR) << "Please use only one of the two flags.";
      return 1;
    }
    FLAGS_out = FLAGS_gcov;
  }

  if (FLAGS_out.empty()) {
    LOG(ERROR) << "Need a name for the generated LLVM profile file.";
    LOG(ERROR) << "Use --gcov or --out to specify an output file.";
    return 1;
  }

  autofdo::ProfileCreator creator(FLAGS_binary);
  if (creator.CreateProfile(FLAGS_profile, FLAGS_profiler, FLAGS_out, "llvm"))
    return 0;
  else
    return -1;
}
