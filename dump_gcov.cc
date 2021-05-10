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

// Read the gcda file and dump the information.

#include "base/commandlineflags.h"
#include "profile_reader.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"

int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(argv[0]);
  absl::ParseCommandLine(argc, argv);

  if (argc != 2) {
    LOG(FATAL) << "Please use: dump_gcov file_path\n";
    return -1;
  }
  devtools_crosstool_autofdo::SymbolMap symbol_map;
  devtools_crosstool_autofdo::AutoFDOProfileReader reader(
      &symbol_map, false);
  reader.ReadFromFile(argv[1]);
  symbol_map.Dump();
  return 0;
}
