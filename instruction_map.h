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

// Class to build a map from instruction address to its information.

#ifndef AUTOFDO_INSTRUCTION_MAP_H_
#define AUTOFDO_INSTRUCTION_MAP_H_

#include <map>
#include <string>
#include <utility>

#include "base/common.h"
#include "symbol_map.h"


namespace autofdo {

class SampleReader;
class Addr2line;

// InstructionMap stores all the disassembled instructions in
// the binary, and maps it to its information.
class InstructionMap {
 public:
  // Arguments:
  //   addr2line: addr2line class, used to get the source stack.
  //   symbol: the symbol map. This object is not const because
  //           we will update the file name of each symbol
  //           according to the debug info of each instruction.
  InstructionMap(Addr2line *addr2line,
                 SymbolMap *symbol)
      : symbol_map_(symbol), addr2line_(addr2line) {
  }

  // Deletes all the InstInfo, which was allocated in BuildInstMap.
  ~InstructionMap();

  // Returns the size of the instruction map.
  uint64 size() const {
    return inst_map_.size();
  }

  // Builds instruction map for a function.
  void BuildPerFunctionInstructionMap(const string &name, uint64 start_addr,
                                      uint64 end_addr);

  // Contains information about each instruction.
  struct InstInfo {
    const SourceInfo &source(int i) const {
      DCHECK(i >= 0 && source_stack.size() > i);
      return source_stack[i];
    }
    SourceStack source_stack;
  };

  typedef map<uint64, InstInfo *> InstMap;
  const InstMap &inst_map() const {
    return inst_map_;
  }

 private:
  // A map from instruction address to its information.
  InstMap inst_map_;

  // A map from symbol name to symbol data.
  SymbolMap *symbol_map_;

  // Addr2line driver which is used to derive source stack.
  Addr2line *addr2line_;

  DISALLOW_COPY_AND_ASSIGN(InstructionMap);
};
}  // namespace autofdo

#endif  // AUTOFDO_INSTRUCTION_MAP_H_
