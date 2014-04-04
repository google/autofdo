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

// Class to build the map from instruction address to its info.

#include "instruction_map.h"

#include <string.h>

#include "addr2line.h"
#include "symbol_map.h"

namespace autofdo {
InstructionMap::~InstructionMap() {
  for (const auto &addr_info : inst_map_) {
    delete addr_info.second;
  }
}

void InstructionMap::BuildPerFunctionInstructionMap(
    const string &name, uint64 start_addr, uint64 end_addr) {
  if (start_addr >= end_addr) {
    return;
  }
  for (uint64 addr = start_addr; addr < end_addr; addr++) {
    InstInfo *info = new InstInfo();
    addr2line_->GetInlineStack(addr, &info->source_stack);
    inst_map_.insert(InstMap::value_type(addr, info));
    if (info->source_stack.size() > 0) {
      symbol_map_->AddSourceCount(name, info->source_stack, 0, 1,
                                  SymbolMap::MAX);
    }
  }
}

}  // namespace autofdo
