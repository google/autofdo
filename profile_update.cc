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

// Updates the AutoFDO profile using the module info and debug info stored
// in the new binary.

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <memory>

#include "base/common.h"
#include "base/logging.h"
#include "addr2line.h"
#include "gcov.h"
#include "profile_reader.h"
#include "profile_writer.h"
#include "symbol_map.h"
#include "module_grouper.h"

DEFINE_string(input, "fbdata.afdo",
              "Input file name of the old profile.");
DEFINE_string(output, "fbdata.afdo",
              "Output file name of the new profile.");
DEFINE_string(binary, "",
              "New binary that has updated module and debug info (built with "
              "-gmlt and -frecord-compilation-info-in-elf).");

using autofdo::SymbolMap;
using autofdo::ModuleGrouper;
using autofdo::Addr2line;
using autofdo::AutoFDOProfileReader;
using autofdo::AutoFDOProfileWriter;

int main(int argc, char **argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  // Step 1. Read the binary to build top-level symbol map to include symbol's
  //         start address and size.
  SymbolMap symbol_map(FLAGS_binary);

  // Step 2. Read in profile to annotate on symbol map and add inlined symbols
  AutoFDOProfileReader reader(&symbol_map, NULL, false);
  reader.ReadFromFile(FLAGS_input);

  // Step 3. Traverse the symbol map to get sampled symbols
  std::map<uint64, uint64> sampled_functions =
     symbol_map.GetLegacySymbolStartAddressSizeMap();

  // Step 4. Read debug info in binary for the sampled symbols
  std::unique_ptr<Addr2line> addr2line(Addr2line::CreateWithSampledFunctions(
      FLAGS_binary, &sampled_functions));

  // Step 5. Use the debug info to update the symbol map (add module info)
  symbol_map.UpdateSymbolMap(addr2line.get(), sampled_functions);
  symbol_map.CalculateThreshold();

  auto grouper = ModuleGrouper::GroupModule(
      FLAGS_binary, GCOV_ELF_SECTION_NAME, &symbol_map);

  AutoFDOProfileWriter writer(&symbol_map, &grouper->module_map(),
                              FLAGS_gcov_version);
  if (!writer.WriteToFile(FLAGS_output)) {
    LOG(FATAL) << "Error writing to " << FLAGS_output;
  }
  return 0;
}
