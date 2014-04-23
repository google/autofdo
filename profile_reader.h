// Read profile from the .afdo file. The format of .afdo file is in
// profile_writer.h.

#ifndef AUTOFDO_PROFILE_READER_H_
#define AUTOFDO_PROFILE_READER_H_

#include <string>
#include <vector>

#include "module_grouper.h"
#include "symbol_map.h"

namespace autofdo {

class SymbolMap;

class AutoFDOProfileReader {
 public:
  // None of the args are owned by this class.
  explicit AutoFDOProfileReader(SymbolMap *symbol_map, ModuleMap *module_map) :
      symbol_map_(symbol_map), module_map_(module_map) {
  }

  explicit AutoFDOProfileReader() :
      symbol_map_(NULL), module_map_(NULL) {}

  void ReadFromFile(const string &output_file);

 private:
  void ReadWorkingSet();
  void ReadModuleGroup();
  void ReadFunctionProfile();
  void ReadSymbolProfile(const SourceStack &stack);
  void ReadNameTable();

  SymbolMap *symbol_map_;
  ModuleMap *module_map_;
  vector<string> names_;
};

}  // namespace autofdo

#endif  // AUTOFDO_PROFILE_READER_H_

