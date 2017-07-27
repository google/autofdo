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
  explicit AutoFDOProfileReader(SymbolMap *symbol_map, ModuleMap *module_map,
                                bool force_update)
      : symbol_map_(symbol_map),
        module_map_(module_map),
        force_update_(force_update) {}

  explicit AutoFDOProfileReader()
      : symbol_map_(NULL), module_map_(NULL), force_update_(false) {}

  void ReadFromFile(const string &output_file);

 private:
  void ReadWorkingSet();
  void ReadModuleGroup();
  void ReadFunctionProfile();
  // Reads in profile recursively. Updates the symbol_map_ if update or
  // force_update_ is true. Otherwise just read in and dump the data.
  // The reason we need "update" is because during profile_update, we first
  // read in symbol_map from the binary, which includes the alias info. Then
  // when we read in profiles from profile, we don't want to update the
  // profiles for the aliased symbols as they all point to the same symbol.
  // With force_update_==true, we know that the ReadSymbolProfile request is
  // not from profile_update but tools like profile_merger and profile_dump,
  // where symbol_map was built purely from profile thus alias symbol info
  // is not available. In that case, we should always update the symbol.
  void ReadSymbolProfile(const SourceStack &stack, bool update);
  void ReadNameTable();

  SymbolMap *symbol_map_;
  ModuleMap *module_map_;
  bool force_update_;
  std::vector<string> names_;
};

}  // namespace autofdo

#endif  // AUTOFDO_PROFILE_READER_H_

