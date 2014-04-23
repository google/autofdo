// Read the gcda file and dump the information.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/common.h"
#include "base/logging.h"
#include "profile_reader.h"
#include "symbol_map.h"
#include "module_grouper.h"

static void PrintModuleProfiles(
    const autofdo::ModuleMap &module_map) {
  for (const auto &name_module : module_map) {
    printf("%s", name_module.first.c_str());
    if (name_module.second.is_exported) {
      printf(" exported");
    }
    printf("(%uKB)", name_module.second.ggc_memory_in_kb);
    if (name_module.second.aux_modules.size() > 0) {
      printf(" (");
      for (const auto &aux_module : name_module.second.aux_modules) {
        printf("%s ", aux_module.c_str());
      }
      printf(" )");
    }
    printf("\n");
  }
}

int main(int argc, char **argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  if (argc != 2) {
    LOG(FATAL) << "Please use: dump_gcov file_path\n";
    return -1;
  }
  autofdo::SymbolMap symbol_map;
  autofdo::ModuleMap module_map;
  autofdo::AutoFDOProfileReader reader(
      &symbol_map, &module_map);
  reader.ReadFromFile(argv[1]);
  symbol_map.Dump();
  PrintModuleProfiles(module_map);
  return 0;
}
