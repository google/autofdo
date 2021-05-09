// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to build the map from instruction address to its info.

#include "instruction_map.h"

#include <string.h>

#include <cstdint>

#include "addr2line.h"
#include "symbol_map.h"

namespace devtools_crosstool_autofdo {
InstructionMap::~InstructionMap() {
  for (const auto &addr_info : inst_map_) {
    delete addr_info.second;
  }
}

void InstructionMap::BuildPerFunctionInstructionMap(const std::string &name,
                                                    uint64_t start_addr,
                                                    uint64_t end_addr) {
  if (start_addr >= end_addr) {
    return;
  }
  for (uint64_t addr = start_addr; addr < end_addr; addr++) {
    InstInfo *info = new InstInfo();
    addr2line_->GetInlineStack(addr, &info->source_stack);
    inst_map_.insert(InstMap::value_type(addr, info));
    if (info->source_stack.size() > 0) {
      symbol_map_->AddSourceCount(name, info->source_stack, 0, 1,
                                  SymbolMap::PERFDATA);
    }
  }
}

}  // namespace devtools_crosstool_autofdo
