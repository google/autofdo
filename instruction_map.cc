// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to build the map from instruction address to its info.

#include "instruction_map.h"

#include <cstdint>
#include <string>

#include "base/logging.h"
#include "addr2line.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {

void InstructionMap::BuildPerFunctionInstructionMap(absl::string_view name,
                                                    uint64_t start_addr,
                                                    uint64_t end_addr) {
  if (start_addr >= end_addr) {
    return;
  }

  // Make sure nobody has set up inst_map_ yet.
  CHECK(inst_map_.empty());

  start_addr_ = start_addr;
  // LOG(INFO) << "Building per function instruction map: " << start_addr << " to "
  //           << end_addr << " (size: " << end_addr - start_addr << ")";
  inst_map_.resize(end_addr - start_addr);
  for (uint64_t addr = start_addr; addr < end_addr; addr++) {
    InstInfo *info = &inst_map_[addr - start_addr];
    addr2line_->GetInlineStack(addr, &info->source_stack);
    if (!info->source_stack.empty()) {
      symbol_map_->AddSourceCount(name, info->source_stack, 0, 1, 1,
                                  SymbolMap::PERFDATA);
    }
  }
}

}  // namespace devtools_crosstool_autofdo
