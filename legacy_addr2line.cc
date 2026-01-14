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

// Class to derive inline stack.

#include "addr2line.h"

#include <string.h>

#include "base/logging.h"
#include "symbolize/bytereader.h"
#include "symbolize/dwarf2reader.h"
#include "symbolize/dwarf3ranges.h"
#include "symbolize/addr2line_inlinestack.h"
#include "symbolize/functioninfo.h"
#include "symbolize/elf_reader.h"
#include "symbol_map.h"

namespace {
void GetSection(const devtools_crosstool_autofdo::SectionMap &sections,
                const char *section_name, const char **data_p, size_t *size_p) {
  const char *data;
  size_t size;

  devtools_crosstool_autofdo::SectionMap::const_iterator section =
      sections.find(section_name);

  if (section == sections.end()) {
    data = NULL;
    size = 0;
  } else {
    data = section->second.first;
    size = section->second.second;
  }

  if (data_p)
    *data_p = data;
  if (size_p)
    *size_p = size;
}
}  // namespace

namespace devtools_crosstool_autofdo {

Addr2line *Addr2line::Create(absl::string_view binary_name) {
    Addr2line *addr2line = new Google3Addr2line(std::string(binary_name));
  if (!addr2line->Prepare()) {
    delete addr2line;
    return NULL;
  } else {
    return addr2line;
  }
}

Google3Addr2line::Google3Addr2line(const std::string& binary_name)
    : Addr2line(binary_name), line_map_(new AddressToLineMap()),
      inline_stack_handler_(NULL), elf_(new ElfReader(binary_name)) {}

Google3Addr2line::~Google3Addr2line() {
  delete line_map_;
  delete elf_;
  if (inline_stack_handler_) {
    delete inline_stack_handler_;
  }
}

bool Google3Addr2line::Prepare() {
  ByteReader reader(ENDIANNESS_LITTLE);
  int width;
  if (elf_->IsElf32File()) {
    width = 4;
  } else if (elf_->IsElf64File()) {
    width = 8;
  } else {
    LOG(ERROR) << "'" << binary_name_ << "' is not an ELF file";
    return false;
  }
  reader.SetAddressSize(width);

  SectionMap sections;
  const char *debug_section_names[] = {
    ".debug_line", ".debug_abbrev", ".debug_info", ".debug_str",
    ".debug_ranges", ".debug_addr", ".debug_rnglists", ".debug_line_str"
  };
  for (const char *section_name : debug_section_names) {
    size_t section_size;
    const char *section_data = elf_->GetSectionByName(section_name,
                                                      &section_size);
    if (section_data == NULL)
      continue;
    sections[section_name] = std::make_pair(section_data, section_size);
  }

  size_t debug_info_size = 0;
  size_t debug_addr_size = 0;
  size_t debug_ranges_size = 0;
  size_t debug_rnglists_size = 0;
  const char *debug_info_data = NULL;
  const char *debug_addr_data = NULL;
  const char *debug_ranges_data = NULL;
  const char *debug_rnglists_data = NULL;
  bool is_rnglists_section = false;
  GetSection(sections, ".debug_info", &debug_info_data, &debug_info_size);
  if (debug_info_data == NULL)
    LOG(WARNING) << "File '" << binary_name_ << "' does not have .debug_info section.";
  GetSection(sections, ".debug_addr", &debug_addr_data, &debug_addr_size);
  GetSection(sections, ".debug_ranges", &debug_ranges_data, &debug_ranges_size);
  GetSection(sections, ".debug_rnglists", &debug_rnglists_data, &debug_rnglists_size);

  if (debug_ranges_data == NULL && debug_rnglists_data == NULL) {
    LOG(WARNING) << "File '" << binary_name_ << "' does not have .debug_ranges nor .debug_rnglists sections.";
  }

  AddressRangeList debug_ranges(debug_ranges_data,
                                debug_ranges_size,
                                debug_rnglists_data,
                                debug_rnglists_size,
                                &reader,
                                debug_addr_data, 
                                debug_addr_size);
  inline_stack_handler_ = new InlineStackHandler(
      &debug_ranges, sections, &reader, nullptr,
      elf_->VaddrOfFirstLoadSegment());

  // Extract the line information
  // If .debug_info section is available, we will locate .debug_line using
  // .debug_info. Otherwise, we'll iterate through .debug_line section,
  // assuming that compilation units are stored continuously in it.
  if (debug_info_size > 0) {
    size_t debug_info_pos = 0;
    while (debug_info_pos < debug_info_size) {
      DirectoryVector dirs;
      FileVector files;
      CULineInfoHandler handler(&files, &dirs, line_map_, nullptr);
      inline_stack_handler_->set_directory_names(&dirs);
      inline_stack_handler_->set_file_names(&files);
      inline_stack_handler_->set_line_handler(&handler);
      CompilationUnit compilation_unit(
          binary_name_, sections, debug_info_pos, &reader,
          inline_stack_handler_);
      debug_info_pos += compilation_unit.Start();
      if (compilation_unit.malformed()) {
        LOG(WARNING) << "File '" << binary_name_ << "' has mangled "
                     << ".debug_info section.";
        // If the compilation unit is malformed, we do not know how
        // big it is, so it is only safe to give up.
        break;
      }
    }
  } else {
    const char *data;
    size_t size;
    GetSection(sections, ".debug_line", &data, &size);
    if (data) {
      size_t pos = 0;
      while (pos < size) {
        DirectoryVector dirs;
        FileVector files;
        CULineInfoHandler handler(&files, &dirs, line_map_);
        LineInfo line(data + pos, size - pos, &reader, &handler);
        uint64_t read = line.Start();
        if (line.malformed()) {
          // If the debug_line section is malformed, we should stop
          LOG(WARNING) << "File '" << binary_name_ << "' has mangled "
                       << ".debug_line section.";
          break;
        }
        if (!read) break;
        pos += read;
      }
    }
    else
      LOG(WARNING) << "File '" << binary_name_ << "' does not have .debug_line section.";
  }
  inline_stack_handler_->PopulateSubprogramsByAddress();

  return true;
}

void Google3Addr2line::GetInlineStack(uint64_t address,
                                      SourceStack *stack) const {
  AddressToLineMap::const_iterator iter = line_map_->upper_bound(address);
  if (iter == line_map_->begin())
    return;
  --iter;
  if (iter->second == 0)
    return;

  const LineIdentifier &LI = line_map_->GetLogical(iter->second);
  if (LI.line == 0)
    return;

  const SubprogramInfo *subprog =
      inline_stack_handler_->GetSubprogramForAddress(address);

  const char *comp_dir = NULL;
  const char *function_name = NULL;
  uint32_t start_line = 0;
  if (subprog != NULL) {
    const SubprogramInfo *declaration =
        inline_stack_handler_->GetDeclaration(subprog);
    function_name = declaration->name().c_str();
    start_line =
        inline_stack_handler_->GetAbstractOrigin(subprog)->callsite_line();
    if (start_line == 0)
      start_line = declaration->callsite_line();
    comp_dir = subprog->comp_directory();
    if (comp_dir == 0) {
      comp_dir = inline_stack_handler_->GetAbstractOrigin(subprog)->comp_directory();
    }
  }

  std::filesystem::path dir_name = LI.file.first;
  if (!dir_name.is_absolute() && comp_dir != nullptr) {
    dir_name = comp_dir / dir_name;
  }

  stack->push_back(SourceInfo(function_name,
                              dir_name,
                              LI.file.second,
                              start_line,
                              LI.line,
                              LI.discriminator));

  while (subprog != NULL && subprog->inlined()) {
    const SubprogramInfo *canonical_parent =
        inline_stack_handler_->GetDeclaration(subprog->parent());
    CHECK(subprog->parent() != NULL);
    uint32_t start_line = inline_stack_handler_->GetAbstractOrigin(
        subprog->parent())->callsite_line();
    if (start_line == 0)
      start_line = canonical_parent->callsite_line();
    if (start_line == 0)
      start_line = subprog->callsite_line();

    dir_name = subprog->callsite_directory();
    if (!dir_name.is_absolute()) {
      if (subprog->comp_directory()) {
        dir_name = subprog->comp_directory() / dir_name;
      } else if (const char *abstract_comp = inline_stack_handler_->GetAbstractOrigin(subprog)->comp_directory()) {
        dir_name = abstract_comp / dir_name;
      }
    }

    stack->push_back(SourceInfo(
        canonical_parent->name().c_str(),
        dir_name,
        subprog->callsite_filename(),
        start_line,
        subprog->callsite_line(),
        subprog->callsite_discr()));
    subprog = subprog->parent();
  }
}
}  // namespace autofdo
