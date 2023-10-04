// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to build a map from instruction address to its information.

#ifndef AUTOFDO_INSTRUCTION_MAP_H_
#define AUTOFDO_INSTRUCTION_MAP_H_

#include <cstdint>
#include <string>
#include <vector>

#include "source_info.h"
#include "symbol_map.h"

namespace devtools_crosstool_autofdo {

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

  // This type is neither copyable nor movable.
  InstructionMap(const InstructionMap &) = delete;
  InstructionMap &operator=(const InstructionMap &) = delete;

  // Builds instruction map for a function.
  void BuildPerFunctionInstructionMap(const std::string &name,
                                      uint64_t start_addr, uint64_t end_addr);

  // Contains information about each instruction.
  struct InstInfo {
    SourceStack source_stack;
  };

  InstInfo *lookup(uint64_t addr) {
    uint64_t idx = addr - start_addr_;  // May underflow, which is OK.
    return idx < inst_map_.size() ? &inst_map_[idx] : nullptr;
  }

 private:
  // A map from instruction address to its information.
  std::vector<InstInfo> inst_map_;

  // The starting address for inst_map_.  inst_map_[pc - start_addr_] maps to
  // the information associated with pc.
  uint64_t start_addr_;

  // A map from symbol name to symbol data.
  SymbolMap *symbol_map_;

  // Addr2line driver which is used to derive source stack.
  Addr2line *addr2line_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_INSTRUCTION_MAP_H_
