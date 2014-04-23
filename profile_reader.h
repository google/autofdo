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

