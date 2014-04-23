// Diff two .afdo files.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/common.h"
#include "base/logging.h"
#include "profile_reader.h"
#include "symbol_map.h"
#include "module_grouper.h"

DEFINE_bool(compare_function, false,
            "whether to compare function level profile");

int main(int argc, char **argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  autofdo::SymbolMap symbol_map_1, symbol_map_2;
  autofdo::ModuleMap module_map;

  if (argc != 3) {
    LOG(FATAL) << "Please specify two files to compare";
  }

  autofdo::AutoFDOProfileReader reader_1(
      &symbol_map_1, &module_map);
  autofdo::AutoFDOProfileReader reader_2(
      &symbol_map_2, &module_map);
  reader_1.ReadFromFile(argv[1]);
  reader_2.ReadFromFile(argv[2]);

  if (FLAGS_compare_function) {
    symbol_map_1.DumpFuncLevelProfileCompare(symbol_map_2);
  }

  printf("%.4f\n", symbol_map_1.Overlap(symbol_map_2));
  return 0;
}
