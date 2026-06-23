// Copyright 2013 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to derive inline stack.

#include "addr2line.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "source_info.h"
#include "third_party/abseil/absl/container/node_hash_map.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugAranges.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"

ABSL_RETIRED_FLAG(bool, use_legacy_symbolizer, false,
                  "whether to use google3 symbolizer");

namespace {
// This maps from a string naming a section to a pair containing a
// the data for the section, and the size of the section.
typedef absl::node_hash_map<std::string, std::pair<const char *, uint64_t>>
    SectionMap;

// Given an ELF binary file with path |filename|, create and return an
// OwningBinary object. If the file does not exist, the OwningBinary object will
// be empty.
llvm::object::OwningBinary<llvm::object::ObjectFile> GetOwningBinary(
    absl::string_view filename) {
  auto object_owning_binary_or_err =
      llvm::object::ObjectFile::createObjectFile(llvm::StringRef(filename));
  if (!object_owning_binary_or_err) {
    return llvm::object::OwningBinary<llvm::object::ObjectFile>();
  }
  return std::move(object_owning_binary_or_err.get());
}
}  // namespace

namespace devtools_crosstool_autofdo {

Addr2line *Addr2line::Create(absl::string_view binary_name) {
  Addr2line *addr2line = new LLVMAddr2line(binary_name);
  if (!addr2line->Prepare()) {
    delete addr2line;
    return nullptr;
  } else {
    return addr2line;
  }
}

LLVMAddr2line::LLVMAddr2line(absl::string_view binary_name)
    : Addr2line(binary_name), binary_(GetOwningBinary(binary_name)) {}

bool LLVMAddr2line::Prepare() {
  if (!binary_.getBinary()) return false;
  dwarf_info_ = llvm::DWARFContext::create(*binary_.getBinary());
  for (auto &unit : dwarf_info_->compile_units()) {
    unit_map_[unit->getOffset()] = unit.get();
  }
  return true;
}

void LLVMAddr2line::GetInlineStack(uint64_t address, SourceStack *stack) const {
  auto cu_iter =
      unit_map_.find(dwarf_info_->getDebugAranges()->findAddress(address));
  if (cu_iter == unit_map_.end()) return;
  const llvm::DWARFDebugLine::LineTable *line_table =
      dwarf_info_->getLineTableForUnit(cu_iter->second);
  if (line_table == nullptr) {
    LOG_EVERY_N(WARNING, 1000) << "Missed line table.";
    return;
  }
  llvm::SmallVector<llvm::DWARFDie, 4> InlinedChain;
  cu_iter->second->getInlinedChainForAddress(address, InlinedChain);

  uint64_t section_index = llvm::object::SectionedAddress::UndefSection;
  if (IsKernelModule(getObject()->getFileName())) {
    // For kernel modules (relocatable objects), line table sequences are often
    // tied to their actual section. We must provide the actual section index
    // (instead of the UndefSection wildcard) to lookupAddress, otherwise it
    // will fail to match sequences that are not marked as UndefSection. We
    // only focus on the .text section to be consistent with symbol_map.cc.
    for (const auto& sec : getObject()->sections()) {
      auto name = sec.getName();
      if (!name) {
        llvm::consumeError(name.takeError());
        continue;
      }
      if (*name == ".text") {
        if (address >= sec.getAddress() &&
            address < sec.getAddress() + sec.getSize()) {
          section_index = sec.getIndex();
        }
        break;
      }
    }
  }
  uint32_t row_index = line_table->lookupAddress({address, section_index});
  uint32_t file = (row_index == -1U ? -1U : line_table->Rows[row_index].File);
  uint32_t line = (row_index == -1U ? 0 : line_table->Rows[row_index].Line);
  uint32_t discriminator =
      (row_index == -1U ? 0 : line_table->Rows[row_index].Discriminator);
  stack->reserve(InlinedChain.size());
  for (const llvm::DWARFDie &FunctionDIE : InlinedChain) {
    const char *function_name =
        FunctionDIE.getSubroutineName(llvm::DINameKind::LinkageName);
    uint32_t start_line = FunctionDIE.getDeclLine();
    llvm::StringRef file_name;
    llvm::StringRef dir_name;
    if (line_table->hasFileAtIndex(file)) {
      const auto &entry = line_table->Prologue.getFileNameEntry(file);
      file_name = llvm::dwarf::toString(entry.Name).value();
      if (entry.DirIdx > 0 &&
          entry.DirIdx <= line_table->Prologue.IncludeDirectories.size())
        dir_name =
            llvm::dwarf::toString(
                line_table->Prologue.IncludeDirectories[entry.DirIdx - 1])
                .value();
    }
    stack->emplace_back(function_name, dir_name, file_name, start_line, line,
                        discriminator);
    uint32_t col;
    FunctionDIE.getCallerFrame(file, line, col, discriminator);
  }
}
}  // namespace devtools_crosstool_autofdo
