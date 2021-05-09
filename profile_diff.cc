// Diff two .afdo files.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "llvm_profile_reader.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/container/node_hash_set.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"

ABSL_FLAG(bool, compare_function, false,
          "whether to compare function level profile");

int main(int argc, char **argv) {
  const char use[] =
      "Usage: profile_diff profile_1.afdo profile_2.afdo\n\n"
      "Outputs the overlap between two profiles (in the range "
      "of [0,1]). If overlap is > 0.9, the two profiles are "
      "considered similar and should provide comparable speedup.";
  absl::SetProgramUsageMessage(use);
  absl::ParseCommandLine(argc, argv);
  devtools_crosstool_autofdo::SymbolMap symbol_map_1, symbol_map_2;

  if (argc != 3) {
    LOG(FATAL) << "Please specify two files to compare";
  }

  absl::node_hash_set<std::string> names;
  devtools_crosstool_autofdo::LLVMProfileReader reader_1(&symbol_map_1, names);
  devtools_crosstool_autofdo::LLVMProfileReader reader_2(&symbol_map_2, names);
  reader_1.ReadFromFile(argv[1]);
  reader_2.ReadFromFile(argv[2]);

  if (absl::GetFlag(FLAGS_compare_function)) {
    symbol_map_1.DumpFuncLevelProfileCompare(symbol_map_2);
  }

  printf("%.4f\n", symbol_map_1.Overlap(symbol_map_2));
  return 0;
}
