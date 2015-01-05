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

#include "symbolize/dwarf2reader.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stack>
#include <utility>

#include "base/logging.h"
#include "symbolize/bytereader.h"
#include "symbolize/bytereader-inl.h"
#include "symbolize/elf_reader.h"
#include "symbolize/line_state_machine.h"

namespace autofdo {

// Read a DWARF2/3 initial length field from START, using READER, and
// report the length in LEN.  Return the actual initial length.

static uint64 ReadInitialLength(const char* start,
                                ByteReader* reader, size_t* len) {
  const uint64 initial_length = reader->ReadFourBytes(start);
  start += 4;

  // In DWARF2/3, if the initial length is all 1 bits, then the offset
  // size is 8 and we need to read the next 8 bytes for the real length.
  if (initial_length == 0xffffffff) {
    reader->SetOffsetSize(8);
    *len = 12;
    return reader->ReadOffset(start);
  } else {
    reader->SetOffsetSize(4);
    *len = 4;
  }
  return initial_length;
}

CompilationUnit::CompilationUnit(const string& path,
                                 const SectionMap& sections, uint64 offset,
                                 ByteReader* reader, Dwarf2Handler* handler)
    : path_(path), offset_from_section_start_(offset), reader_(reader),
      sections_(sections), handler_(handler), abbrevs_(NULL),
      string_buffer_(NULL), string_buffer_length_(0),
      str_offsets_buffer_(NULL), str_offsets_buffer_length_(0),
      addr_buffer_(NULL), addr_buffer_length_(0),
      is_split_dwarf_(false), dwo_id_(0), dwo_name_(), ranges_base_(0),
      addr_base_(0), have_checked_for_dwp_(false), dwp_path_(),
      dwp_byte_reader_(NULL), dwp_reader_(NULL), malformed_(false) {}

CompilationUnit::~CompilationUnit() {
  if (abbrevs_) delete abbrevs_;
  if (dwp_reader_) delete dwp_reader_;
  if (dwp_byte_reader_) delete dwp_byte_reader_;
}

// Initialize a compilation unit from a .dwo or .dwp file.
// In this case, we need the .debug_addr section from the
// executable file that contains the corresponding skeleton
// compilation unit.  We also inherit the Dwarf2Handler from
// the executable file, and call it as if we were still
// processing the original compilation unit.

void CompilationUnit::SetSplitDwarf(const char* addr_buffer,
                                    uint64 addr_buffer_length,
                                    uint64 addr_base,
                                    uint64 ranges_base) {
  is_split_dwarf_ = true;
  addr_buffer_ = addr_buffer;
  addr_buffer_length_ = addr_buffer_length;
  addr_base_ = addr_base;
  ranges_base_ = ranges_base;
}

// Read a DWARF2/3 abbreviation section.
// Each abbrev consists of a abbreviation number, a tag, a byte
// specifying whether the tag has children, and a list of
// attribute/form pairs.
// The list of forms is terminated by a 0 for the attribute, and a
// zero for the form.  The entire abbreviation section is terminated
// by a zero for the code.

void CompilationUnit::ReadAbbrevs() {
  if (abbrevs_)
    return;

  // First get the debug_abbrev section
  SectionMap::const_iterator iter = sections_.find(".debug_abbrev");
  CHECK(iter != sections_.end());

  if (abbrevs_) {
    delete abbrevs_;
  }
  abbrevs_ = new vector<Abbrev>;
  abbrevs_->resize(1);

  // The only way to CHECK whether we are reading over the end of the
  // buffer would be to first compute the size of the leb128 data by
  // reading it, then go back and read it again.
  const char* abbrev_start = iter->second.first +
                                      header_.abbrev_offset;
  const char* abbrevptr = abbrev_start;
  const uint64 abbrev_length = iter->second.second - header_.abbrev_offset;

  while (1) {
    CompilationUnit::Abbrev abbrev;
    size_t len;
    const uint32 number = reader_->ReadUnsignedLEB128(abbrevptr, &len);

    if (number == 0)
      break;
    abbrev.number = number;
    abbrevptr += len;

    DCHECK(abbrevptr < abbrev_start + abbrev_length);
    const uint32 tag = reader_->ReadUnsignedLEB128(abbrevptr, &len);
    abbrevptr += len;
    abbrev.tag = static_cast<enum DwarfTag>(tag);

    DCHECK(abbrevptr < abbrev_start + abbrev_length);
    abbrev.has_children = reader_->ReadOneByte(abbrevptr);
    abbrevptr += 1;

    DCHECK(abbrevptr < abbrev_start + abbrev_length);

    while (1) {
      const uint32 nametemp = reader_->ReadUnsignedLEB128(abbrevptr, &len);
      abbrevptr += len;

      DCHECK(abbrevptr < abbrev_start + abbrev_length);
      const uint32 formtemp = reader_->ReadUnsignedLEB128(abbrevptr, &len);
      abbrevptr += len;
      if (nametemp == 0 && formtemp == 0)
        break;

      const enum DwarfAttribute name =
        static_cast<enum DwarfAttribute>(nametemp);
      const enum DwarfForm form = static_cast<enum DwarfForm>(formtemp);
      abbrev.attributes.push_back(std::make_pair(name, form));
    }
    CHECK(abbrev.number == abbrevs_->size());
    abbrevs_->push_back(abbrev);
  }
}

// Skips a single DIE's attributes.
const char* CompilationUnit::SkipDIE(const char* start,
                                              const Abbrev& abbrev) {
  for (AttributeList::const_iterator i = abbrev.attributes.begin();
       i != abbrev.attributes.end();
       i++)  {
    start = SkipAttribute(start, i->second);
  }
  return start;
}

// Skips a single attribute form's data.
const char* CompilationUnit::SkipAttribute(const char* start,
                                                    enum DwarfForm form) {
  size_t len;

  switch (form) {
    case DW_FORM_indirect:
      form = static_cast<enum DwarfForm>(reader_->ReadUnsignedLEB128(start,
                                                                     &len));
      start += len;
      return SkipAttribute(start, form);
      break;

    case DW_FORM_flag_present:
      return start;
      break;

    case DW_FORM_data1:
    case DW_FORM_flag:
    case DW_FORM_ref1:
      return start + 1;
      break;
    case DW_FORM_ref2:
    case DW_FORM_data2:
      return start + 2;
      break;
    case DW_FORM_ref4:
    case DW_FORM_data4:
      return start + 4;
      break;
    case DW_FORM_ref8:
    case DW_FORM_ref_sig8:
    case DW_FORM_data8:
      return start + 8;
      break;
    case DW_FORM_string:
      return start + strlen(start) + 1;
      break;
    case DW_FORM_udata:
    case DW_FORM_ref_udata:
    case DW_FORM_GNU_str_index:
    case DW_FORM_GNU_addr_index:
      reader_->ReadUnsignedLEB128(start, &len);
      return start + len;
      break;

    case DW_FORM_sdata:
      reader_->ReadSignedLEB128(start, &len);
      return start + len;
      break;
    case DW_FORM_addr:
      return start + reader_->AddressSize();
      break;
    case DW_FORM_ref_addr:
      // DWARF2 and 3 differ on whether ref_addr is address size or
      // offset size.
      if (header_.version == 2) {
        return start + reader_->AddressSize();
      } else {
        return start + reader_->OffsetSize();
      }
      break;

    case DW_FORM_block1:
      return start + 1 + reader_->ReadOneByte(start);
      break;
    case DW_FORM_block2:
      return start + 2 + reader_->ReadTwoBytes(start);
      break;
    case DW_FORM_block4:
      return start + 4 + reader_->ReadFourBytes(start);
      break;
    case DW_FORM_block:
    case DW_FORM_exprloc: {
      uint64 size = reader_->ReadUnsignedLEB128(start, &len);
      return start + size + len;
    }
      break;
    case DW_FORM_strp:
    case DW_FORM_sec_offset:
        return start + reader_->OffsetSize();
      break;
    default:
      LOG(FATAL) << "Unhandled form type";
  }
  LOG(FATAL) << "Unhandled form type";
  return NULL;
}

// Read a DWARF2/3 header.
// The header is variable length in DWARF3 (and DWARF2 as extended by
// most compilers), and consists of an length field, a version number,
// the offset in the .debug_abbrev section for our abbrevs, and an
// address size.
void CompilationUnit::ReadHeader() {
  const char* headerptr = buffer_;
  size_t initial_length_size;

  if (headerptr + 4 >= buffer_ + buffer_length_) {
    malformed_ = true;
    return;
  }
  const uint64 initial_length = ReadInitialLength(headerptr, reader_,
                                                  &initial_length_size);
  headerptr += initial_length_size;
  header_.length = initial_length;
  if (header_.length == 0
      || headerptr + 2 >= buffer_ + buffer_length_) {
    malformed_ = true;
    return;
  }

  header_.version = reader_->ReadTwoBytes(headerptr);
  if (header_.version < 2 || header_.version > 4) {
    malformed_ = true;
    return;
  }
  headerptr += 2;

  if (headerptr + reader_->OffsetSize() >= buffer_ + buffer_length_) {
    malformed_ = true;
    return;
  }
  header_.abbrev_offset = reader_->ReadOffset(headerptr);
  headerptr += reader_->OffsetSize();

  if (headerptr + 1 >= buffer_ + buffer_length_) {
    malformed_ = true;
    return;
  }
  header_.address_size = reader_->ReadOneByte(headerptr);
  if (header_.address_size != 4 && header_.address_size != 8) {
    malformed_ = true;
    return;
  }
  reader_->SetAddressSize(header_.address_size);
  headerptr += 1;

  after_header_ = headerptr;

  // This check ensures that we don't have to do checking during the
  // reading of DIEs. header_.length does not include the size of the
  // initial length.
  if (buffer_ + initial_length_size + header_.length >
      buffer_ + buffer_length_) {
    malformed_ = true;
    return;
  }
}

uint64 CompilationUnit::Start(uint64 offset) {
  // Reset all members except for construction parameters and *dwp*.
  if (abbrevs_) {
    delete abbrevs_;
  }
  abbrevs_ = NULL;

  offset_from_section_start_ = offset;

  string_buffer_ = NULL;
  string_buffer_length_ = 0;

  str_offsets_buffer_ = NULL;
  str_offsets_buffer_length_ = 0;

  addr_buffer_ = NULL;
  addr_buffer_length_ = 0;

  after_header_ = NULL;
  malformed_ = false;

  dwo_id_ = 0;
  dwo_name_ = NULL;

  ranges_base_ = 0;
  addr_base_ = 0;

  return Start();
}

uint64 CompilationUnit::Start() {
  // First get the debug_info section
  SectionMap::const_iterator iter = sections_.find(".debug_info");
  CHECK(iter != sections_.end());

  // Set up our buffer
  buffer_ = iter->second.first + offset_from_section_start_;
  buffer_length_ = iter->second.second - offset_from_section_start_;

  // Read the header
  ReadHeader();

  // If the header is malformed, the data may be uninitialized and we
  // don't know how to proceed in this section, so return the size of
  // the section so the loop will stop.
  if (malformed()) {
    return iter->second.second;;
  }

  // Figure out the real length from the end of the initial length to
  // the end of the compilation unit, since that is the value we
  // return.
  uint64 ourlength = header_.length;
  if (reader_->OffsetSize() == 8)
    ourlength += 12;
  else
    ourlength += 4;

  if (is_split_dwarf_) {
    // If the user does not want it, just return.
    if (!handler_->StartSplitCompilationUnit(offset_from_section_start_,
                                            header_.length)) {
      return ourlength;
    }
  } else {
    // If the user does not want it, just return.
    if (!handler_->StartCompilationUnit(offset_from_section_start_,
                                        reader_->AddressSize(),
                                        reader_->OffsetSize(),
                                        header_.length,
                                        header_.version)) {
      return ourlength;
    }
  }

  // Otherwise, continue by reading our abbreviation entries.
  ReadAbbrevs();

  // Set the string section if we have one.
  iter = sections_.find(".debug_str");
  if (iter != sections_.end()) {
    string_buffer_ = iter->second.first;
    string_buffer_length_ = iter->second.second;
  }

  // Set the string offsets section if we have one.
  iter = sections_.find(".debug_str_offsets");
  if (iter != sections_.end()) {
    str_offsets_buffer_ = iter->second.first;
    str_offsets_buffer_length_ = iter->second.second;
  }

  // Set the address section if we have one.
  iter = sections_.find(".debug_addr");
  if (iter != sections_.end()) {
    addr_buffer_ = iter->second.first;
    addr_buffer_length_ = iter->second.second;
  }

  // Now that we have our abbreviations, start processing DIE's.
  ProcessDIEs();

  return ourlength;
}

// If one really wanted, you could merge SkipAttribute and
// ProcessAttribute
// This is all boring data manipulation and calling of the handler.
const char* CompilationUnit::ProcessAttribute(
    uint64 dieoffset, const char* start, enum DwarfAttribute attr,
    enum DwarfForm form) {
  size_t len;

  switch (form) {
    // DW_FORM_indirect is never used because it is such a space
    // waster.
    case DW_FORM_indirect:
      form = static_cast<enum DwarfForm>(reader_->ReadUnsignedLEB128(start,
                                                                     &len));
      start += len;
      return ProcessAttribute(dieoffset, start, attr, form);
      break;

    case DW_FORM_flag_present:
      ProcessAttributeUnsigned(dieoffset, attr, form, 1);
      return start;
      break;
    case DW_FORM_data1:
    case DW_FORM_flag:
    case DW_FORM_ref1:
      ProcessAttributeUnsigned(dieoffset, attr, form,
                                         reader_->ReadOneByte(start));
      return start + 1;
      break;
    case DW_FORM_ref2:
    case DW_FORM_data2:
      ProcessAttributeUnsigned(dieoffset, attr, form,
                                         reader_->ReadTwoBytes(start));
      return start + 2;
      break;
    case DW_FORM_ref4:
    case DW_FORM_data4:
      ProcessAttributeUnsigned(dieoffset, attr, form,
                                         reader_->ReadFourBytes(start));
      return start + 4;
      break;
    case DW_FORM_ref8:
    case DW_FORM_ref_sig8:
    case DW_FORM_data8:
      ProcessAttributeUnsigned(dieoffset, attr, form,
                                         reader_->ReadEightBytes(start));
      return start + 8;
      break;
    case DW_FORM_string: {
      const char* str = start;
      ProcessAttributeString(dieoffset, attr, form,
                                       str);
      return start + strlen(str) + 1;
      break;
    }
    case DW_FORM_udata:
    case DW_FORM_ref_udata:
      ProcessAttributeUnsigned(dieoffset, attr, form,
                                         reader_->ReadUnsignedLEB128(start,
                                                                     &len));
      return start + len;
      break;

    case DW_FORM_sdata:
      ProcessAttributeSigned(dieoffset, attr, form,
                                      reader_->ReadSignedLEB128(start, &len));
      return start + len;
      break;
    case DW_FORM_addr:
      ProcessAttributeUnsigned(dieoffset, attr, form,
                                         reader_->ReadAddress(start));
      return start + reader_->AddressSize();
      break;
    case DW_FORM_ref_addr:
      // DWARF2 and 3 differ on whether ref_addr is address size or
      // offset size.
      if (header_.version == 2) {
        ProcessAttributeUnsigned(dieoffset, attr, form,
                                           reader_->ReadAddress(start));
        return start + reader_->AddressSize();
      } else {
        ProcessAttributeUnsigned(dieoffset, attr, form,
                                           reader_->ReadOffset(start));
        return start + reader_->OffsetSize();
      }
      break;
    case DW_FORM_sec_offset:
      ProcessAttributeUnsigned(dieoffset, attr, form,
                                         reader_->ReadOffset(start));
      return start + reader_->OffsetSize();
      break;

    case DW_FORM_block1: {
      uint64 datalen = reader_->ReadOneByte(start);
      ProcessAttributeBuffer(dieoffset, attr, form, start + 1,
                                      datalen);
      return start + 1 + datalen;
      break;
    }
    case DW_FORM_block2: {
      uint64 datalen = reader_->ReadTwoBytes(start);
      ProcessAttributeBuffer(dieoffset, attr, form, start + 2,
                                      datalen);
      return start + 2 + datalen;
      break;
    }
    case DW_FORM_block4: {
      uint64 datalen = reader_->ReadFourBytes(start);
      ProcessAttributeBuffer(dieoffset, attr, form, start + 4,
                                      datalen);
      return start + 4 + datalen;
      break;
    }
    case DW_FORM_block:
    case DW_FORM_exprloc: {
      uint64 datalen = reader_->ReadUnsignedLEB128(start, &len);
      ProcessAttributeBuffer(dieoffset, attr, form, start + len,
                                      datalen);
      return start + datalen + len;
      break;
    }
    case DW_FORM_strp: {
      CHECK(string_buffer_ != NULL);

      const uint64 offset = reader_->ReadOffset(start);
      if (offset >= string_buffer_length_) {
        LOG(WARNING) << "offset is out of range.  offset=" << offset
                     << " string_buffer_length_=" << string_buffer_length_;
        return NULL;
      }

      const char* str = string_buffer_ + offset;
      ProcessAttributeString(dieoffset, attr, form,
                                       str);
      return start + reader_->OffsetSize();
      break;
    }
    case DW_FORM_GNU_str_index: {
      CHECK(string_buffer_ != NULL);
      CHECK(str_offsets_buffer_ != NULL);

      uint64 str_index = reader_->ReadUnsignedLEB128(start, &len);
      const char* offset_ptr =
          str_offsets_buffer_ + str_index * reader_->OffsetSize();
      const uint64 offset = reader_->ReadOffset(offset_ptr);
      if (offset >= string_buffer_length_) {
        LOG(WARNING) << "offset is out of range.  offset=" << offset
                     << " string_buffer_length_=" << string_buffer_length_;
        return NULL;
      }

      const char* str = string_buffer_ + offset;
      ProcessAttributeString(dieoffset, attr, form,
                                       str);
      return start + len;
      break;
    }
    case DW_FORM_GNU_addr_index: {
      CHECK(addr_buffer_ != NULL);
      uint64 addr_index = reader_->ReadUnsignedLEB128(start, &len);
      const char* addr_ptr =
          addr_buffer_ + addr_base_ + addr_index * reader_->AddressSize();
      ProcessAttributeUnsigned(dieoffset, attr, form,
                                         reader_->ReadAddress(addr_ptr));
      return start + len;
      break;
    }
    default:
      LOG(FATAL) << "Unhandled form type";
  }
  LOG(FATAL) << "Unhandled form type";
  return NULL;
}

const char* CompilationUnit::ProcessDIE(uint64 dieoffset,
                                                 const char* start,
                                                 const Abbrev& abbrev) {
  for (AttributeList::const_iterator i = abbrev.attributes.begin();
       i != abbrev.attributes.end();
       i++)  {
    start = ProcessAttribute(dieoffset, start, i->first, i->second);
    if (start == NULL) {
      break;
    }
  }

  // If this is a skeleton compilation unit generated with split DWARF,
  // we need to find the full compilation unit in a .dwo or .dwp file.
  if (abbrev.tag == DW_TAG_compile_unit
      && !is_split_dwarf_
      && dwo_name_ != NULL)
    ProcessSplitDwarf();

  return start;
}

void CompilationUnit::ProcessDIEs() {
  const char* dieptr = after_header_;
  size_t len;

  // lengthstart is the place the length field is based on.
  // It is the point in the header after the initial length field
  const char* lengthstart = buffer_;

  // In 64 bit dwarf, the initial length is 12 bytes, because of the
  // 0xffffffff at the start.
  if (reader_->OffsetSize() == 8)
    lengthstart += 12;
  else
    lengthstart += 4;

  stack<uint64> die_stack;

  while (dieptr < (lengthstart + header_.length)) {
    // We give the user the absolute offset from the beginning of
    // debug_info, since they need it to deal with ref_addr forms.
    uint64 absolute_offset = (dieptr - buffer_) + offset_from_section_start_;

    uint64 abbrev_num = reader_->ReadUnsignedLEB128(dieptr, &len);

    dieptr += len;

    // Abbrev == 0 represents the end of a list of children, or padding between
    // sections.
    if (abbrev_num == 0) {
      if (!die_stack.empty()) {
        const uint64 offset = die_stack.top();
        die_stack.pop();
        handler_->EndDIE(offset);
      }
      continue;
    }

    const Abbrev& abbrev = abbrevs_->at(abbrev_num);
    const enum DwarfTag tag = abbrev.tag;
    if (!handler_->StartDIE(absolute_offset, tag, abbrev.attributes)) {
      dieptr = SkipDIE(dieptr, abbrev);
    } else {
      dieptr = ProcessDIE(absolute_offset, dieptr, abbrev);
      if (dieptr == NULL) {
        break;
      }
    }

    if (abbrev.has_children) {
      die_stack.push(absolute_offset);
    } else {
      handler_->EndDIE(absolute_offset);
    }
  }
}

// Check for a valid ELF file and return the Address size.
// Returns 0 if not a valid ELF file.

inline int GetElfWidth(const ElfReader& elf) {
  if (elf.IsElf32File())
    return 4;
  if (elf.IsElf64File())
    return 8;
  return 0;
}

void CompilationUnit::ProcessSplitDwarf() {
  struct stat statbuf;

  if (!have_checked_for_dwp_) {
    // Look for a .dwp file in the same directory as the executable.
    have_checked_for_dwp_ = true;
    dwp_path_ = path_ + ".dwp";
    if (stat(dwp_path_.c_str(), &statbuf) == 0) {
      ElfReader* elf = new ElfReader(dwp_path_);
      int width = GetElfWidth(*elf);
      if (width != 0) {
        dwp_byte_reader_ = new ByteReader(ENDIANNESS_NATIVE);
        dwp_byte_reader_->SetAddressSize(width);
        dwp_reader_ = new DwpReader(*dwp_byte_reader_, elf);
        dwp_reader_->Initialize();
      } else {
        LOG(WARNING) << "File '" << dwp_path_ << "' is not an ELF file.";
        delete elf;
      }
    }
  }
  bool found_in_dwp = false;
  if (dwp_reader_ != NULL) {
    // If we have a .dwp file, read the debug sections for the requested CU.
    SectionMap sections;
    dwp_reader_->ReadDebugSectionsForCU(dwo_id_, &sections);
    if (!sections.empty()) {
      found_in_dwp = true;
      CompilationUnit dwp_comp_unit(dwp_path_, sections, 0, dwp_byte_reader_,
                                    handler_);
      dwp_comp_unit.SetSplitDwarf(addr_buffer_, addr_buffer_length_, addr_base_,
                                  ranges_base_);
      dwp_comp_unit.Start();
      if (dwp_comp_unit.malformed())
        LOG(WARNING) << "File '" << dwp_path_ << "' has mangled "
                     << ".debug_info.dwo section.";
    }
  }
  if (!found_in_dwp) {
    // If no .dwp file, try to open the .dwo file.
    if (stat(dwo_name_, &statbuf) == 0) {
      ElfReader elf(dwo_name_);
      int width = GetElfWidth(elf);
      if (width != 0) {
        ByteReader reader(ENDIANNESS_NATIVE);
        reader.SetAddressSize(width);
        SectionMap sections;
        ReadDebugSectionsFromDwo(&elf, &sections);
        CompilationUnit dwo_comp_unit(dwo_name_, sections, 0, &reader,
                                      handler_);
        dwo_comp_unit.SetSplitDwarf(addr_buffer_, addr_buffer_length_,
                                    addr_base_, ranges_base_);
        dwo_comp_unit.Start();
        if (dwo_comp_unit.malformed())
          LOG(WARNING) << "File '" << dwo_name_ << "' has mangled "
                       << ".debug_info.dwo section.";
      } else {
        LOG(WARNING) << "File '" << dwo_name_ << "' is not an ELF file.";
      }
    } else if (dwp_reader_ == NULL) {
      LOG(WARNING) << "Cannot open file '" << dwo_name_ << "'.";
    }
  }
}

void CompilationUnit::ReadDebugSectionsFromDwo(ElfReader* elf_reader,
                                              SectionMap* sections) {
  static const char* section_names[] = {
    ".debug_abbrev",
    ".debug_info",
    ".debug_str_offsets",
    ".debug_str"
  };
  for (int i = 0; i < arraysize(section_names); ++i) {
    string base_name = section_names[i];
    string dwo_name = base_name + ".dwo";
    size_t section_size;
    const char* section_data = elf_reader->GetSectionByName(dwo_name,
                                                            &section_size);
    if (section_data != NULL)
      sections->insert(std::make_pair(
          base_name, std::make_pair(section_data, section_size)));
  }
}

DwpReader::DwpReader(const ByteReader& byte_reader, ElfReader* elf_reader)
    : elf_reader_(elf_reader), byte_reader_(byte_reader),
      cu_index_(NULL), cu_index_size_(0), string_buffer_(NULL),
      string_buffer_size_(0), version_(0), ncolumns_(0), nunits_(0),
      nslots_(0), phash_(NULL), pindex_(NULL), shndx_pool_(NULL),
      offset_table_(NULL), size_table_(NULL), abbrev_data_(NULL),
      abbrev_size_(0), info_data_(NULL), info_size_(0),
      str_offsets_data_(NULL), str_offsets_size_(0) {}

DwpReader::~DwpReader() {
  if (elf_reader_) delete elf_reader_;
}

void DwpReader::Initialize() {
  cu_index_ = elf_reader_->GetSectionByName(".debug_cu_index",
                                            &cu_index_size_);
  if (cu_index_ == NULL)
    return;

  // The .debug_str.dwo section is shared by all CUs in the file.
  string_buffer_ = elf_reader_->GetSectionByName(".debug_str.dwo",
                                                 &string_buffer_size_);

  version_ = byte_reader_.ReadFourBytes(cu_index_);

  if (version_ == 1) {
    nslots_ = byte_reader_.ReadFourBytes(cu_index_ + 3 * sizeof(uint32));
    phash_ = cu_index_ + 4 * sizeof(uint32);
    pindex_ = phash_ + nslots_ * sizeof(uint64);
    shndx_pool_ = pindex_ + nslots_ * sizeof(uint32);
    if (shndx_pool_ >= cu_index_ + cu_index_size_) {
      LOG(WARNING) << ".debug_cu_index is corrupt";
      version_ = 0;
    }
  } else if (version_ == 2) {
    ncolumns_ = byte_reader_.ReadFourBytes(cu_index_ + sizeof(uint32));
    nunits_ = byte_reader_.ReadFourBytes(cu_index_ + 2 * sizeof(uint32));
    nslots_ = byte_reader_.ReadFourBytes(cu_index_ + 3 * sizeof(uint32));
    phash_ = cu_index_ + 4 * sizeof(uint32);
    pindex_ = phash_ + nslots_ * sizeof(uint64);
    offset_table_ = pindex_ + nslots_ * sizeof(uint32);
    size_table_ = offset_table_ + ncolumns_ * (nunits_ + 1) * sizeof(uint32);
    abbrev_data_ = elf_reader_->GetSectionByName(".debug_abbrev.dwo",
                                                 &abbrev_size_);
    info_data_ = elf_reader_->GetSectionByName(".debug_info.dwo", &info_size_);
    str_offsets_data_ = elf_reader_->GetSectionByName(".debug_str_offsets.dwo",
                                                      &str_offsets_size_);
    if (size_table_ >= cu_index_ + cu_index_size_) {
      LOG(WARNING) << ".debug_cu_index is corrupt";
      version_ = 0;
    }
  } else {
    LOG(WARNING) << "Unexpected version number in .dwp file.";
  }
}

void DwpReader::ReadDebugSectionsForCU(uint64 dwo_id,
                                       SectionMap* sections) {
  if (version_ == 1) {
    int slot = LookupCU(dwo_id);
    if (slot == -1) {
      LOG(WARNING) << "dwo_id 0x" << std::hex << dwo_id
                   << " not found in .dwp file.";
      return;
    }

    // The index table points to the section index pool, where we
    // can read a list of section indexes for the debug sections
    // for the CU whose dwo_id we are looking for.
    int index = byte_reader_.ReadFourBytes(pindex_ + slot * sizeof(uint32));
    const char* shndx_list = shndx_pool_ + index * sizeof(uint32);
    for (;;) {
      if (shndx_list >= cu_index_ + cu_index_size_) {
        LOG(WARNING) << ".debug_cu_index is corrupt";
        version_ = 0;
        return;
      }
      unsigned int shndx = byte_reader_.ReadFourBytes(shndx_list);
      shndx_list += sizeof(uint32);
      if (shndx == 0)
        break;
      const char* section_name = elf_reader_->GetSectionName(shndx);
      size_t section_size;
      const char* section_data;
      // We're only interested in these four debug sections.
      // The section names in the .dwo file end with ".dwo", but we
      // add them to the sections table with their normal names.
      if (strncmp(section_name, ".debug_abbrev", 13) == 0) {
        section_data = elf_reader_->GetSectionByIndex(shndx, &section_size);
        sections->insert(std::make_pair(
            ".debug_abbrev", std::make_pair(section_data, section_size)));
      } else if (strncmp(section_name, ".debug_info", 11) == 0) {
        section_data = elf_reader_->GetSectionByIndex(shndx, &section_size);
        sections->insert(std::make_pair(
            ".debug_info", std::make_pair(section_data, section_size)));
      } else if (strncmp(section_name, ".debug_str_offsets", 18) == 0) {
        section_data = elf_reader_->GetSectionByIndex(shndx, &section_size);
        sections->insert(std::make_pair(
            ".debug_str_offsets", std::make_pair(section_data, section_size)));
      }
    }
    sections->insert(std::make_pair(
        ".debug_str", std::make_pair(string_buffer_, string_buffer_size_)));
  } else if (version_ == 2) {
    uint32 index = LookupCUv2(dwo_id);
    if (index == 0) {
      LOG(WARNING) << "dwo_id 0x" << std::hex << dwo_id
                   << " not found in .dwp file.";
      return;
    }

    // The index points to a row in each of the section offsets table
    // and the section size table, where we can read the offsets and sizes
    // of the contributions to each debug section from the CU whose dwo_id
    // we are looking for. Row 0 of the section offsets table has the
    // section ids for each column of the table. The size table begins
    // with row 1.
    const char* id_row = offset_table_;
    const char* offset_row = offset_table_ + index * ncolumns_ * sizeof(uint32);
    const char* size_row =
        size_table_ + (index - 1) * ncolumns_ * sizeof(uint32);
    if (size_row + ncolumns_ * sizeof(uint32) > cu_index_ + cu_index_size_) {
      LOG(WARNING) << ".debug_cu_index is corrupt";
      version_ = 0;
      return;
    }
    for (int col = 0; col < ncolumns_; ++col) {
      uint32 section_id =
          byte_reader_.ReadFourBytes(id_row + col * sizeof(uint32));
      uint32 offset =
          byte_reader_.ReadFourBytes(offset_row + col * sizeof(uint32));
      uint32 size =
          byte_reader_.ReadFourBytes(size_row + col * sizeof(uint32));
      if (section_id == DW_SECT_ABBREV) {
        sections->insert(std::make_pair(
            ".debug_abbrev", std::make_pair(abbrev_data_ + offset, size)));
      } else if (section_id == DW_SECT_INFO) {
        sections->insert(std::make_pair(
            ".debug_info", std::make_pair(info_data_ + offset, size)));
      } else if (section_id == DW_SECT_STR_OFFSETS) {
        sections->insert(
            std::make_pair(".debug_str_offsets",
                           std::make_pair(str_offsets_data_ + offset, size)));
      }
    }
    sections->insert(std::make_pair(
        ".debug_str", std::make_pair(string_buffer_, string_buffer_size_)));
  }
}

int DwpReader::LookupCU(uint64 dwo_id) {
  uint32 slot = static_cast<uint32>(dwo_id) & (nslots_ - 1);
  uint64 probe = byte_reader_.ReadEightBytes(phash_ + slot * sizeof(uint64));
  if (probe != 0 && probe != dwo_id) {
    uint32 secondary_hash =
        (static_cast<uint32>(dwo_id >> 32) & (nslots_ - 1)) | 1;
    do {
      slot = (slot + secondary_hash) & (nslots_ - 1);
      probe = byte_reader_.ReadEightBytes(phash_ + slot * sizeof(uint64));
    } while (probe != 0 && probe != dwo_id);
  }
  if (probe == 0)
    return -1;
  return slot;
}

uint32 DwpReader::LookupCUv2(uint64 dwo_id) {
  uint32 slot = static_cast<uint32>(dwo_id) & (nslots_ - 1);
  uint64 probe = byte_reader_.ReadEightBytes(phash_ + slot * sizeof(uint64));
  uint32 index = byte_reader_.ReadFourBytes(pindex_ + slot * sizeof(uint32));
  if (index != 0 && probe != dwo_id) {
    uint32 secondary_hash =
        (static_cast<uint32>(dwo_id >> 32) & (nslots_ - 1)) | 1;
    do {
      slot = (slot + secondary_hash) & (nslots_ - 1);
      probe = byte_reader_.ReadEightBytes(phash_ + slot * sizeof(uint64));
      index = byte_reader_.ReadFourBytes(pindex_ + slot * sizeof(uint32));
    } while (index != 0 && probe != dwo_id);
  }
  return index;
}

LineInfo::LineInfo(const char* buffer, uint64 buffer_length,
                   ByteReader* reader, LineInfoHandler* handler):
    handler_(handler), reader_(reader), buffer_(buffer),
    buffer_length_(buffer_length), malformed_(false) {
  header_.std_opcode_lengths = NULL;
}

uint64 LineInfo::Start() {
  ReadHeader();
  if (malformed()) {
    // Return the buffer_length_ so callers will not process further
    // in this section.
    return buffer_length_;
  }
  ReadLines();
  return after_header_ - buffer_;
}

bool LineInfo::AdvanceLinePtr(int incr,  const char **lineptr) {
  const char *buffer_end = buffer_ + buffer_length_;
  if (*lineptr + incr >= buffer_end) {
    // The '>=' comparison above is somewhat bogus: it assumes that we
    // are going to necessarily read from the resulting lineptr.
    // It would be better to check for 'lineptr < buffer_end' before
    // reading from lineptr instead of after incrementing it.
    malformed_ = true;
    return false;
  }
  *lineptr += incr;
  return true;
}

// The header for a debug_line section is mildly complicated, because
// the line info is very tightly encoded.
void LineInfo::ReadHeader() {
  const char* lineptr = buffer_;
  size_t initial_length_size;

  const uint64 initial_length = ReadInitialLength(lineptr, reader_,
                                                  &initial_length_size);

  if (!AdvanceLinePtr(initial_length_size, &lineptr)) {
    return;
  }
  header_.total_length = initial_length;
  if (buffer_ + initial_length_size + header_.total_length
      > buffer_ + buffer_length_) {
    malformed_ = true;
    return;
  }

  // Address size *must* be set by CU ahead of time.
  if (reader_->AddressSize() == 0) {
    malformed_ = true;
    return;
  }

  header_.version = reader_->ReadTwoBytes(lineptr);
  if (!AdvanceLinePtr(2, &lineptr)) {
    return;
  }
  if (header_.version < 2 || header_.version > 4) {
    malformed_ = true;
    return;
  }

  header_.prologue_length = reader_->ReadOffset(lineptr);
  if (!AdvanceLinePtr(reader_->OffsetSize(), &lineptr)) {
    return;
  }

  header_.min_insn_length = reader_->ReadOneByte(lineptr);
  if (!AdvanceLinePtr(1, &lineptr)) {
    return;
  }

  if (header_.version >= 4) {
    header_.max_ops_per_insn = reader_->ReadOneByte(lineptr);
    if (!AdvanceLinePtr(1, &lineptr)) {
      return;
    }
  } else {
    header_.max_ops_per_insn = 1;
  }

  header_.default_is_stmt = reader_->ReadOneByte(lineptr);
  if (!AdvanceLinePtr(1, &lineptr)) {
    return;
  }

  header_.line_base = *reinterpret_cast<const int8*>(lineptr);
  if (!AdvanceLinePtr(1, &lineptr)) {
    return;
  }

  header_.line_range = reader_->ReadOneByte(lineptr);
  if (!AdvanceLinePtr(1, &lineptr)) {
    return;
  }

  header_.opcode_base = reader_->ReadOneByte(lineptr);
  if (!AdvanceLinePtr(1, &lineptr)) {
    return;
  }

  header_.std_opcode_lengths = new vector<unsigned char>;
  header_.std_opcode_lengths->resize(header_.opcode_base + 1);
  (*header_.std_opcode_lengths)[0] = 0;
  for (int i = 1; i < header_.opcode_base; i++) {
    (*header_.std_opcode_lengths)[i] = reader_->ReadOneByte(lineptr);
    if (!AdvanceLinePtr(1, &lineptr)) {
      return;
    }
  }

  // It is legal for the directory entry table to be empty.
  if (*lineptr) {
    uint32 dirindex = 1;
    while (*lineptr) {
      const char* dirname = lineptr;
      handler_->DefineDir(dirname, dirindex);
      if (!AdvanceLinePtr(strlen(dirname) + 1, &lineptr)) {
        return;
      }
      dirindex++;
    }
  }
  if (!AdvanceLinePtr(1, &lineptr)) {
    return;
  }

  // It is also legal for the file entry table to be empty.
  if (*lineptr) {
    uint32 fileindex = 1;
    size_t len;
    while (*lineptr) {
      const char* filename = lineptr;
      if (!AdvanceLinePtr(strlen(filename) + 1, &lineptr)) {
        return;
      }

      uint64 dirindex = reader_->ReadUnsignedLEB128(lineptr, &len);
      if (!AdvanceLinePtr(len, &lineptr)) {
        return;
      }

      uint64 mod_time = reader_->ReadUnsignedLEB128(lineptr, &len);
      if (!AdvanceLinePtr(len, &lineptr)) {
        return;
      }

      uint64 filelength = reader_->ReadUnsignedLEB128(lineptr, &len);
      if (!AdvanceLinePtr(len, &lineptr)) {
        return;
      }
      handler_->DefineFile(filename, fileindex, dirindex, mod_time,
                           filelength);
      fileindex++;
    }
  }
  if (++lineptr > buffer_ + buffer_length_) {
    malformed_ = true;
    return;
  }

  after_header_ = lineptr;
}

/* static */
bool LineInfo::ProcessOneOpcode(ByteReader* reader,
                                LineInfoHandler* handler,
                                const struct LineInfoHeader &header,
                                const char* start,
                                struct LineStateMachine* lsm,
                                size_t* len,
                                uintptr_t pc) {
  size_t oplen = 0;
  size_t templen;
  uint8 opcode = reader->ReadOneByte(start);
  oplen++;
  start++;

  // If the opcode is great than the opcode_base, it is a special
  // opcode. Most line programs consist mainly of special opcodes.
  if (opcode >= header.opcode_base) {
    opcode -= header.opcode_base;
    const int64 advance_address = (opcode / header.line_range)
                                  * header.min_insn_length;
    const int64 advance_line = (opcode % header.line_range)
                               + header.line_base;

    lsm->address += advance_address;
    lsm->line_num += advance_line;
    lsm->basic_block = true;
    *len = oplen;
    return true;
  }

  // Otherwise, we have the regular opcodes
  switch (opcode) {
    case DW_LNS_copy: {
      lsm->basic_block = false;
      *len = oplen;
      return true;
    }

    case DW_LNS_advance_pc: {
      uint64 advance_address = reader->ReadUnsignedLEB128(start, &templen);
      oplen += templen;
      lsm->address += header.min_insn_length * advance_address;
    }
      break;
    case DW_LNS_advance_line: {
      const int64 advance_line = reader->ReadSignedLEB128(start, &templen);
      oplen += templen;
      lsm->line_num += advance_line;
    }
      break;
    case DW_LNS_set_file: {
      const uint64 fileno = reader->ReadUnsignedLEB128(start, &templen);
      oplen += templen;
      lsm->file_num = fileno;
    }
      break;
    case DW_LNS_set_column: {
      const uint64 colno = reader->ReadUnsignedLEB128(start, &templen);
      oplen += templen;
      lsm->column_num = colno;
    }
      break;
    case DW_LNS_negate_stmt: {
      lsm->is_stmt = !lsm->is_stmt;
    }
      break;
    case DW_LNS_set_basic_block: {
      lsm->basic_block = true;
    }
      break;
    case DW_LNS_fixed_advance_pc: {
      const uint16 advance_address = reader->ReadTwoBytes(start);
      oplen += 2;
      lsm->address += advance_address;
    }
      break;
    case DW_LNS_const_add_pc: {
      const int64 advance_address = header.min_insn_length
                                    * ((255 - header.opcode_base)
                                       / header.line_range);
      lsm->address += advance_address;
    }
      break;
    case DW_LNS_extended_op: {
      const size_t extended_op_len = reader->ReadUnsignedLEB128(start,
                                                                &templen);
      start += templen;
      oplen += templen + extended_op_len;

      const uint64 extended_op = reader->ReadOneByte(start);
      start++;

      switch (extended_op) {
        case DW_LNE_end_sequence: {
          lsm->end_sequence = true;
          *len = oplen;
          return true;
        }
          break;
        case DW_LNE_set_address: {
          uint64 address = reader->ReadAddress(start);
          lsm->address = address;
        }
          break;
        case DW_LNE_define_file: {
          const char* filename  = start;

          templen = strlen(filename) + 1;
          start += templen;

          uint64 dirindex = reader->ReadUnsignedLEB128(start, &templen);
          start += templen;

          const uint64 mod_time = reader->ReadUnsignedLEB128(start,
                                                             &templen);
          start += templen;

          const uint64 filelength = reader->ReadUnsignedLEB128(start,
                                                               &templen);
          start += templen;

          if (handler) {
            handler->DefineFile(filename, -1, dirindex, mod_time,
                                filelength);
          }
        }
          break;
        case DW_LNE_set_discriminator: {
          const uint64 discriminator = reader->ReadUnsignedLEB128(start,
                                                                  &templen);
          lsm->discriminator = static_cast<uint32>(discriminator);
        }
          break;
      }
    }
      break;

    default: {
      // Ignore unknown opcode silently
      if (header.std_opcode_lengths) {
        for (int i = 0; i < (*header.std_opcode_lengths)[opcode]; i++) {
          size_t templen;
          reader->ReadUnsignedLEB128(start, &templen);
          start += templen;
          oplen += templen;
        }
      }
    }
      break;
  }
  *len = oplen;
  return false;
}

void LineInfo::ReadLines() {
  struct LineStateMachine lsm;

  // lengthstart is the place the length field is based on.
  // It is the point in the header after the initial length field
  const char* lengthstart = buffer_;

  // In 64 bit dwarf, the initial length is 12 bytes, because of the
  // 0xffffffff at the start.
  if (reader_->OffsetSize() == 8)
    lengthstart += 12;
  else
    lengthstart += 4;

  const char* lineptr = after_header_;
  while (lineptr < lengthstart + header_.total_length) {
    lsm.Reset(header_.default_is_stmt);
    while (!lsm.end_sequence) {
      size_t oplength;
      bool add_line = ProcessOneOpcode(reader_, handler_, header_,
                                       lineptr, &lsm, &oplength, -1);
      if (add_line) {
        handler_->AddLine(lsm.address, lsm.file_num, lsm.line_num,
                          lsm.column_num, lsm.discriminator);
        lsm.basic_block = false;
        lsm.discriminator = 0;
      }
      lineptr += oplength;
    }
  }

  after_header_ = lengthstart + header_.total_length;
}

}  // namespace autofdo
