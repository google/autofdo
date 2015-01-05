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
void GetSection(const autofdo::SectionMap &sections,
                const char *section_name, const char **data_p, size_t *size_p,
                const string &file_name, const char *error) {
  const char *data;
  size_t size;

  autofdo::SectionMap::const_iterator section =
      sections.find(section_name);

  if (section == sections.end()) {
    LOG(WARNING) << "File '" << file_name << "' has no " << section_name
                 << " section.  " << error;
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

namespace autofdo {

Addr2line *Addr2line::Create(const string &binary_name) {
  return CreateWithSampledFunctions(binary_name, NULL);
}

Addr2line *Addr2line::CreateWithSampledFunctions(
    const string &binary_name, const map<uint64, uint64> *sampled_functions) {
  Addr2line *addr2line = new Google3Addr2line(binary_name, sampled_functions);
  if (!addr2line->Prepare()) {
    delete addr2line;
    return NULL;
  } else {
    return addr2line;
  }
}

Google3Addr2line::Google3Addr2line(const string &binary_name,
                                   const map<uint64, uint64> *sampled_functions)
    : Addr2line(binary_name), line_map_(new AddressToLineMap()),
      inline_stack_handler_(NULL), elf_(new ElfReader(binary_name)),
      sampled_functions_(sampled_functions) {}

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
    ".debug_line", ".debug_abbrev", ".debug_info", ".debug_line", ".debug_str",
    ".debug_ranges", ".debug_addr"
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
  size_t debug_ranges_size = 0;
  const char *debug_ranges_data = NULL;
  GetSection(sections, ".debug_info", NULL, &debug_info_size, binary_name_, "");
  GetSection(sections, ".debug_ranges", &debug_ranges_data,
             &debug_ranges_size, binary_name_, "");
  AddressRangeList debug_ranges(debug_ranges_data,
                                                debug_ranges_size,
                                                &reader);
  inline_stack_handler_ = new InlineStackHandler(
      &debug_ranges, sections, &reader, sampled_functions_,
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
      CULineInfoHandler handler(&files, &dirs, line_map_, sampled_functions_);
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
    GetSection(sections, ".debug_line", &data, &size, binary_name_, "");
    if (data) {
      size_t pos = 0;
      while (pos < size) {
        DirectoryVector dirs;
        FileVector files;
        CULineInfoHandler handler(&files, &dirs, line_map_);
        LineInfo line(data + pos, size - pos, &reader, &handler);
        uint64 read = line.Start();
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
  }
  inline_stack_handler_->PopulateSubprogramsByAddress();

  return true;
}

void Google3Addr2line::GetInlineStack(uint64 address,
                                      SourceStack *stack) const {
  AddressToLineMap::const_iterator iter = line_map_->upper_bound(address);
  if (iter == line_map_->begin())
    return;
  --iter;
  if (iter->second.line == 0)
    return;

  const SubprogramInfo *subprog =
      inline_stack_handler_->GetSubprogramForAddress(address);

  const char *function_name = NULL;
  uint32 start_line = 0;
  if (subprog != NULL) {
    const SubprogramInfo *declaration =
        inline_stack_handler_->GetDeclaration(subprog);
    function_name = declaration->name().c_str();
    start_line =
        inline_stack_handler_->GetAbstractOrigin(subprog)->callsite_line();
    if (start_line == 0)
      start_line = declaration->callsite_line();
  }

  stack->push_back(SourceInfo(function_name,
                              iter->second.file.first,
                              iter->second.file.second,
                              start_line,
                              iter->second.line,
                              iter->second.discriminator));

  while (subprog != NULL && subprog->inlined()) {
    const SubprogramInfo *canonical_parent =
        inline_stack_handler_->GetDeclaration(subprog->parent());
    CHECK(subprog->parent() != NULL);
    uint32 start_line = inline_stack_handler_->GetAbstractOrigin(
        subprog->parent())->callsite_line();
    if (start_line == 0)
      start_line = canonical_parent->callsite_line();
    if (start_line == 0)
      start_line = subprog->callsite_line();
    stack->push_back(SourceInfo(
        canonical_parent->name().c_str(),
        subprog->callsite_directory(),
        subprog->callsite_filename(),
        start_line,
        subprog->callsite_line(),
        subprog->callsite_discr()));
    subprog = subprog->parent();
  }
}
}  // namespace autofdo
