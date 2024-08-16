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
#include "symbolize/dwarf2enums.h"
#include "symbolize/elf_reader.h"
#include "symbolize/line_state_machine.h"
#include "symbolize/addr2line_inlinestack.h"
#include "symbolize/index_helper.h"

namespace devtools_crosstool_autofdo {

CompilationUnit::CompilationUnit(const string& path,
                                 const SectionMap& sections, uint64 offset,
                                 ByteReader* reader, Dwarf2Handler* handler)
    : path_(path), offset_from_section_start_(offset), reader_(reader),
      sections_(sections), handler_(handler), abbrevs_(NULL),
      string_buffer_(NULL), string_buffer_length_(0),
      line_str_buffer_(NULL), line_str_buffer_length_(0),
      str_offsets_buffer_(NULL), str_offsets_buffer_length_(0),
      addr_buffer_(NULL), addr_buffer_length_(0),
      is_split_dwarf_(false), dwo_name_(),
      skeleton_dwo_id_(0), have_checked_for_dwp_(false), dwp_path_(),
      dwp_byte_reader_(NULL), dwp_reader_(NULL), malformed_(false) {}

CompilationUnit::CompilationUnit(const string& path, const string& dwp_path,
                                 const SectionMap& sections, uint64 offset,
                                 ByteReader* reader, Dwarf2Handler* handler)
    : path_(path), offset_from_section_start_(offset), reader_(reader),
      sections_(sections), handler_(handler), abbrevs_(),
      string_buffer_(NULL), string_buffer_length_(0),
      line_str_buffer_(NULL), line_str_buffer_length_(0),
      str_offsets_buffer_(NULL), str_offsets_buffer_length_(0),
      addr_buffer_(NULL), addr_buffer_length_(0),
      is_split_dwarf_(false), dwo_name_(),
      skeleton_dwo_id_(0), have_checked_for_dwp_(false), dwp_path_(dwp_path),
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
                                    uint64 dwo_id) {
  is_split_dwarf_ = true;
  addr_buffer_ = addr_buffer;
  addr_buffer_length_ = addr_buffer_length;
  skeleton_dwo_id_ = dwo_id;
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
      uint32 value = 0;
      if (form == DW_FORM_implicit_const) {
          DCHECK(abbrevptr < abbrev_start + abbrev_length);
          value = reader_->ReadUnsignedLEB128(abbrevptr, &len);
          abbrevptr += len;
      }

      AttributeSpec spec;
      spec.name = name;
      spec.form = form;
      spec.value = value;

      abbrev.attributes.push_back(spec);
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
    start = SkipAttribute(start, i->form);
  }
  return start;
}

// Skips a single attribute form's data.
const char* CompilationUnit::SkipAttribute(const char* start, enum DwarfForm form) {
  size_t len;

  switch (form) {
    case DW_FORM_indirect:
      form = static_cast<enum DwarfForm>(reader_->ReadUnsignedLEB128(start,
                                                                     &len));
      start += len;
      return SkipAttribute(start, form);
      break;
    case DW_FORM_implicit_const:
    case DW_FORM_flag_present:
      return start;
      break;
    case DW_FORM_data1:
    case DW_FORM_flag:
    case DW_FORM_ref1:
    case DW_FORM_strx1:
    case DW_FORM_addrx1:
      return start + 1;
      break;
    case DW_FORM_ref2:
    case DW_FORM_data2:
    case DW_FORM_strx2:
    case DW_FORM_addrx2:
      return start + 2;
      break;
    case DW_FORM_strx3:
    case DW_FORM_addrx3:
      return start + 3;
      break;
    case DW_FORM_ref4:
    case DW_FORM_data4:
    case DW_FORM_strx4:
    case DW_FORM_addrx4:
    case DW_FORM_ref_sup4:
      return start + 4;
      break;
    case DW_FORM_ref8:
    case DW_FORM_ref_sig8:
    case DW_FORM_data8:
    case DW_FORM_ref_sup8:
      return start + 8;
      break;
    case DW_FORM_data16:
      return start + 16;
      break;
    case DW_FORM_string:
      return start + strlen(start) + 1;
      break;
    case DW_FORM_udata:
    case DW_FORM_ref_udata:
    case DW_FORM_GNU_str_index:
    case DW_FORM_GNU_addr_index:
    case DW_FORM_strx:
    case DW_FORM_addrx:
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
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
    case DW_FORM_strp_sup:
    case DW_FORM_sec_offset:
        return start + reader_->OffsetSize();
      break;
    default:
      LOG(FATAL) << "Unhandled form type";
  }
  LOG(FATAL) << "Unhandled form type";
  return NULL;
}

void CompilationUnit::ReadDebugOffsetTableHeader(const char* str_offset_ptr,
                                                 uint64 str_offset_length) {
  const char* headerptr = str_offsets_buffer_ = str_offset_ptr;
  str_offsets_buffer_length_ = str_offset_length;
  size_t initial_length_size;

  if (headerptr + 4 >= str_offset_ptr + str_offset_length) {
    malformed_ = true;
    return;
  }

  const uint64 initial_length = reader_->ReadInitialLength(headerptr, &initial_length_size);
  headerptr += initial_length_size;

  uint16 version = reader_->ReadTwoBytes(headerptr);
  if (header_.version != version) {
    malformed_ = true;
    return;
  }
  headerptr += 2;

  // discard the padding.
  headerptr += 2;

  handler_->set_str_offset_base(headerptr - str_offsets_buffer_);


  if(str_offsets_buffer_length_ < 0) {
    malformed_ = true;
    return;
  }                                                 
}

void CompilationUnit::ReadDebugAddressTableHeader(const char* addr_ptr,
                                                 uint64 addr_length) {
  const char* headerptr = addr_buffer_ = addr_ptr;
  addr_buffer_length_ = addr_length;
  size_t initial_length_size;

  if (headerptr + 4 >= addr_ptr + addr_length) {
    malformed_ = true;
    return;
  }

  const uint64 initial_length = reader_->ReadInitialLength(headerptr, &initial_length_size);
  headerptr += initial_length_size;

  uint16 version = reader_->ReadTwoBytes(headerptr);
  headerptr += 2;
  if (header_.version != version) {
    malformed_ = true;
    return;
  }
  
  uint8 address_size = reader_->ReadOneByte(headerptr);
  headerptr += 1;

  if (address_size != header_.address_size) {
    LOG(WARNING) << "Address size in .debug_addr header is " << address_size << " while address size in .debug_info header is " << header_.address_size;
    malformed_ = true;
    return;
  }

  uint8 segment_selector_size = reader_->ReadOneByte(headerptr);
  headerptr += 1;

  handler_->set_addr_base(headerptr - addr_buffer_);

  if(addr_buffer_length_ < 0) {
    malformed_ = true;
    return;
  }   
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
  const uint64 initial_length = reader_->ReadInitialLength(headerptr, &initial_length_size);
  headerptr += initial_length_size;
  header_.length = initial_length;
  if (header_.length == 0
      || headerptr + 2 >= buffer_ + buffer_length_) {
    malformed_ = true;
    return;
  }

  header_.version = reader_->ReadTwoBytes(headerptr);
  if (header_.version < 2 || header_.version > 5) {
    malformed_ = true;
    return;
  }
  headerptr += 2;

  if (header_.version == 5) {
      if (headerptr + 1 >= buffer_ + buffer_length_) {
          malformed_ = true;
          return;
      }
      header_.unit_type = reader_->ReadOneByte(headerptr);
      if (header_.unit_type != DwarfUnitType::DW_UT_compile &&
          header_.unit_type != DwarfUnitType::DW_UT_skeleton &&
          header_.unit_type != DwarfUnitType::DW_UT_split_compile) 
      {
          LOG(INFO) << "Only full compilation unit and skeleton unit types are supported.";
          malformed_ = true;
          return;
      }
      headerptr += 1;
  }

  if (header_.version == 5) {
      if (!ReadAddressSize(&headerptr)) {
          return;
      }

      if (!ReadAbbrevOffset(&headerptr)) {
          return;
      }

      if (header_.unit_type == DwarfUnitType::DW_UT_skeleton ||
          header_.unit_type == DwarfUnitType::DW_UT_split_compile) {
        header_.dwo_id = reader_->ReadEightBytes(headerptr);
        headerptr += 8;
      }
  }
  else {
      if (!ReadAbbrevOffset(&headerptr)) {
          return;
      }

      if (!ReadAddressSize(&headerptr)) {
          return;
      }
  }

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

bool CompilationUnit::ReadAddressSize(const char** headerptr) {
    if (*headerptr + 1 >= buffer_ + buffer_length_) {
        malformed_ = true;
        return false;
    }
    header_.address_size = reader_->ReadOneByte(*headerptr);
    if (header_.address_size != 4 && header_.address_size != 8) {
        malformed_ = true;
        return false;
    }
    reader_->SetAddressSize(header_.address_size);
    *headerptr = *headerptr + 1;
    return true;
}

bool CompilationUnit::ReadAbbrevOffset(const char** headerptr) {
    if (*headerptr + reader_->OffsetSize() >= buffer_ + buffer_length_) {
        malformed_ = true;
        return false;
    }
    header_.abbrev_offset = reader_->ReadOffset(*headerptr);
    *headerptr = *headerptr + reader_->OffsetSize();
    return true;
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

  line_str_buffer_ = NULL;
  line_str_buffer_length_ = 0;

  str_offsets_buffer_ = NULL;
  str_offsets_buffer_length_ = 0;

  addr_buffer_ = NULL;
  addr_buffer_length_ = 0;

  after_header_ = NULL;
  malformed_ = false;

  dwo_name_ = NULL;
  skeleton_dwo_id_ = 0;

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

  // Set the string section specific to the line number table if we have one.
  iter = sections_.find(".debug_line_str");
  if (iter != sections_.end()) {
    line_str_buffer_ = iter->second.first;
    line_str_buffer_length_ = iter->second.second;
  }

  // Set the string offsets section if we have one.
  iter = sections_.find(".debug_str_offsets");
  if (iter != sections_.end()) {
    if (header_.version == 5) {
      ReadDebugOffsetTableHeader(iter->second.first, iter->second.second);
    }
    else {
      str_offsets_buffer_ = iter->second.first;
      str_offsets_buffer_length_ = iter->second.second;      
    }
  }

  if (malformed()) {
    return iter->second.second;;
  }

  // Set the address section if we have one.
  iter = sections_.find(".debug_addr");
  if (iter != sections_.end()) {
    if (header_.version == 5) {
      ReadDebugAddressTableHeader(iter->second.first, iter->second.second);
    }
    else {
      addr_buffer_ = iter->second.first;
      addr_buffer_length_ = iter->second.second;      
    }
  }

  if (malformed()) {
    return iter->second.second;;
  }

  // Now that we have our abbreviations, start processing DIE's.
  ProcessDIEs();

  // If this is a skeleton compilation unit generated with split DWARF,
  // and the client needs the full debug info, we need to find the full
  // compilation unit in a .dwo or .dwp file.
  if (!is_split_dwarf_
      && dwo_name_ != NULL
      && handler_->NeedSplitDebugInfo()) {
    ProcessSplitDwarf();
    handler_->EndSplitCompilationUnit();
  }

  return ourlength;
}

// If one really wanted, you could merge SkipAttribute and
// ProcessAttribute
// This is all boring data manipulation and calling of the handler.
const char* CompilationUnit::ProcessAttribute(
    uint64 dieoffset, const char* start, enum DwarfAttribute attr,
    enum DwarfForm form, uint32 value) {
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
    case DW_FORM_data16:
      // It is possible to implement 128bit constant using 
      // a ReadSixteenBytes function and "typedef __int128 uint128".
      // But not all compilers support __int128.
      LOG(WARNING) << "We don't support 128bit as constant."; 
      return start + 16;
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
    case DW_FORM_line_strp: {
      CHECK(line_str_buffer_ != NULL);
      const uint64 offset = reader_->ReadOffset(start);
      ProcessAttributeString(dieoffset, attr, form,
                             get_string_with_offsets(line_str_buffer_, line_str_buffer_length_, offset));
      return start + reader_->OffsetSize();
      break;
    }
    case DW_FORM_strp: {
      CHECK(string_buffer_ != NULL);
      const uint64 offset = reader_->ReadOffset(start);
      ProcessAttributeString(dieoffset, attr, form,
                             get_string_with_offsets(string_buffer_, string_buffer_length_, offset));
      return start + reader_->OffsetSize();
      break;
    }
    case DW_FORM_strx1:
    case DW_FORM_strx2:
    case DW_FORM_strx3:
    case DW_FORM_strx4:
    case DW_FORM_strx:
    case DW_FORM_GNU_str_index: {
      CHECK(string_buffer_ != NULL);
      CHECK(str_offsets_buffer_ != NULL);
      uint64 str_index = 0;
      size_t len = 0;
        switch (form) {
          case DW_FORM_strx1: {
            str_index = reader_->ReadOneByte(start);
            len += 1;
            break; 
          }
          case DW_FORM_strx2: {
            str_index = reader_->ReadTwoBytes(start);
            len += 2;
            break;  
          }
          case DW_FORM_strx3: {
            str_index = reader_->ReadThreeBytes(start);
            len += 3;
            break;    
          }
          case DW_FORM_strx4: {
            str_index = reader_->ReadFourBytes(start);
            len += 4;
            break;
          }
          case DW_FORM_strx:
          case DW_FORM_GNU_str_index: {
            str_index = reader_->ReadUnsignedLEB128(start, &len);
            break;
          }
        }
      const char* offset_ptr =
            get_offset_pointer_in_str_offsets_table(str_index,
                                                    str_offsets_buffer_,
                                                    handler_->get_str_offset_base(),
                                                    reader_->OffsetSize(),
                                                    header_.version);
      const uint64 offset = reader_->ReadOffset(offset_ptr);
      ProcessAttributeString(dieoffset, attr, form,
                             get_string_with_offsets(string_buffer_, string_buffer_length_, offset));
      return start + len;
      break; 
    }
    case DW_FORM_rnglistx: {
      uint64 rng_index = reader_->ReadUnsignedLEB128(start, &len);
      ProcessAttributeUnsigned(dieoffset, attr, form, rng_index);
      return start + len;
      break;
    }
    case DW_FORM_addrx1: 
    case DW_FORM_addrx2: 
    case DW_FORM_addrx3: 
    case DW_FORM_addrx4:
    case DW_FORM_addrx: 
    case DW_FORM_GNU_addr_index: {
      CHECK(addr_buffer_ != NULL);
      uint64 addr_index = 0;
      size_t len;
      switch (form) {
        case DW_FORM_addrx1: {
          addr_index = reader_->ReadOneByte(start);
          len += 1;
            break;
        }
        case DW_FORM_addrx2: {
          uint64 addr_index = reader_->ReadTwoBytes(start);
          len += 2;
            break;
        }
        case DW_FORM_addrx3: {
          addr_index = reader_->ReadThreeBytes(start);
          len += 3;
            break;
        }
        case DW_FORM_addrx4: {
          addr_index = reader_->ReadFourBytes(start);
          len += 4;
            break;
        }
        case DW_FORM_GNU_addr_index: 
        case DW_FORM_addrx: {
          addr_index = reader_->ReadUnsignedLEB128(start, &len);
          break;
        }
      }
      uint64 v = reader_->ReadAddress(
                          get_address_pointer_from_address_table(addr_buffer_,
                                                                handler_->get_addr_base(),
                                                                addr_index,
                                                                reader_->AddressSize())
                          );
      ProcessAttributeUnsigned(dieoffset, attr, form, v);
      return start + 1;
      break;  
    }
    case DW_FORM_implicit_const: {
      CHECK(header_.version == 5);
      ProcessAttributeSigned(dieoffset, attr, form, value);
      return start;
      break;
    }
    default:
      LOG(FATAL) << "Unhandled form type";
  }
  LOG(FATAL) << "Unhandled form type";
  return NULL;
}

void CompilationUnit::ProcessAttributeUnsigned(uint64 offset,
                                                enum DwarfAttribute attr,
                                                enum DwarfForm form,
                                                uint64 data) {
    if (attr == DW_AT_GNU_dwo_id) // In DWARF 5, this attribute has moved to header field.
      header_.dwo_id = data;
    handler_->ProcessAttributeUnsigned(offset, attr, form, data);
  }

const char* CompilationUnit::ProcessDIE(uint64 dieoffset,
                                        const char* start,
                                        const Abbrev& abbrev) {
  for (AttributeList::const_iterator i = abbrev.attributes.begin();
       i != abbrev.attributes.end();
       i++)  {
      start = ProcessAttribute (
          dieoffset, start, i->name, i->form,
          i->form == DwarfForm::DW_FORM_implicit_const ? i->value : 0);
    if (start == NULL) {
      break;
    }
  }

  // If this is a compilation unit in a split DWARF object, verify that
  // the dwo_id matches. If it does not match, we will ignore this
  // compilation unit.
  if (abbrev.tag == DW_TAG_compile_unit
      && is_split_dwarf_
      && header_.dwo_id != skeleton_dwo_id_) {
    LOG(WARNING) << "dwo_id 0x" << std::hex << skeleton_dwo_id_
                 << " does not match .dwo file '" << path_ << "'.";
    return NULL;
  }

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
    have_checked_for_dwp_ = true;
    if (dwp_path_.empty()) {
      // Look for a .dwp file in the same directory as the executable.
      dwp_path_ = path_ + ".dwp";
    }
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
    dwp_reader_->ReadDebugSectionsForCU(header_.dwo_id, &sections);
    if (!sections.empty()) {
      found_in_dwp = true;
      CompilationUnit dwp_comp_unit(dwp_path_, sections, 0,
                                    dwp_byte_reader_, handler_);
      dwp_comp_unit.SetSplitDwarf(addr_buffer_, addr_buffer_length_, header_.dwo_id);
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

        auto iter = sections.find(".debug_rnglists");
        size_t debug_ranges_size = 0;
        const char *debug_ranges_data = NULL;
        size_t debug_rnglist_size = 0;
        const char *debug_rnglist_data = NULL;
        if (iter != sections.end()) {
          debug_rnglist_data = iter->second.first;
          debug_rnglist_size = iter->second.second;
          AddressRangeList *debug_ranges = 
                new AddressRangeList(debug_ranges_data,
                                      debug_ranges_size,
                                      debug_rnglist_data,
                                      debug_rnglist_size,
                                      &reader,
                                      addr_buffer_, 
                                      addr_buffer_length_);
          InlineStackHandler *inlinestackhandler = dynamic_cast<InlineStackHandler*>(handler_);
          if(!inlinestackhandler)  {
            LOG(FATAL) << "It is not possbile to process the .debug_rnglists sections in seperate .dwo files without InlineStackHandler.";
          }
          inlinestackhandler->set_address_range_list(debug_ranges);
        }

        
        CompilationUnit dwo_comp_unit(dwo_name_, sections, 0, &reader,
                                      handler_);
        dwo_comp_unit.SetSplitDwarf(addr_buffer_, addr_buffer_length_, header_.dwo_id);
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
    ".debug_str",
    ".debug_rnglists"
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
    handler_(handler), reader_(reader),
    buffer_(buffer), buffer_length_(buffer_length),
    line_str_buffer_(NULL), line_str_buffer_length_(0),
    str_buffer_(NULL), str_buffer_length_(0),
    str_offsets_buffer_(NULL), str_offsets_buffer_length_(0), str_offset_base_(0),
    after_header_(NULL),
    logicals_start_(NULL), actuals_start_(NULL),
    malformed_(false) {
  header_.std_opcode_lengths = NULL;
}

LineInfo::LineInfo(const char* buffer, uint64 buffer_length,
                   const char* line_str_buffer, uint64 line_str_buffer_length,
                   const char* str_buffer, uint64 str_buffer_length,
                   const char* str_offsets_buffer, uint64 str_offsets_buffer_length, 
                   uint64 str_offset_base, ByteReader* reader, LineInfoHandler* handler):
    handler_(handler), reader_(reader), buffer_(buffer),
    buffer_length_(buffer_length), line_str_buffer_(line_str_buffer),
    line_str_buffer_length_(line_str_buffer_length), str_buffer_(str_buffer),
    str_buffer_length_(str_buffer_length), str_offsets_buffer_(str_offsets_buffer),
    str_offsets_buffer_length_(str_offsets_buffer_length), str_offset_base_(str_offset_base),
    after_header_(NULL), logicals_start_(NULL), actuals_start_(NULL),
    malformed_(false) {
  header_.std_opcode_lengths = NULL;
}

uint64 LineInfo::Start() {
  handler_->StartCU();
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

bool LineInfo::ReadTypesAndForms(const char** lineptr,
    uint32* content_types, uint32* content_forms, uint32 max_types,
    uint32* format_count) {
  size_t len;

  uint32 count = reader_->ReadUnsignedLEB128(*lineptr, &len);
  if (!AdvanceLinePtr(len, lineptr)) {
    return false;
  }
  if (count < 1 || count > max_types) {
    return false;
  }
  for (uint32 col = 0; col < count; ++col) {
    content_types[col] = reader_->ReadUnsignedLEB128(*lineptr, &len);
    if (!AdvanceLinePtr(len, lineptr)) {
      return false;
    }
    content_forms[col] = reader_->ReadUnsignedLEB128(*lineptr, &len);
    if (!AdvanceLinePtr(len, lineptr)) {
      return false;
    }
  }
  *format_count = count;
  return true;
}

bool LineInfo::ReadStringForm(uint32 form, const char** dirname,
    const char** lineptr) {

  switch (form) {
    case DW_FORM_string: {
      *dirname = *lineptr;
      if (!AdvanceLinePtr(strlen(*dirname) + 1, lineptr)) {
        return false;
      }
      break;
    }
    case DW_FORM_line_strp: {
      uint64 offset = reader_->ReadOffset(*lineptr);
      if (!AdvanceLinePtr(reader_->OffsetSize(), lineptr)) {
        return false;
      }
      if (line_str_buffer_ == NULL) {
        return false;
      }
      *dirname = line_str_buffer_ + offset;
      break;
    }
    case DW_FORM_strx1:
    case DW_FORM_strx2:
    case DW_FORM_strx3:
    case DW_FORM_strx4:
    case DW_FORM_strx: {
      CHECK(str_buffer_ != NULL);
      CHECK(str_offsets_buffer_ != NULL);
      size_t len = 0;
      uint64 str_index = 0;
      switch (form) {
        case DW_FORM_strx1: {
          str_index = reader_->ReadOneByte(*lineptr);
          len += 1;
          break;
        }
        case DW_FORM_strx2: {
          str_index = reader_->ReadTwoBytes(*lineptr);
          len += 2;
          break;
        }
        case DW_FORM_strx3: {
          str_index = reader_->ReadThreeBytes(*lineptr);
          len += 3;
          break;
        }
        case DW_FORM_strx4: {
          str_index = reader_->ReadFourBytes(*lineptr);
          len += 4;
          break;
        }
        case DW_FORM_strx: {
          str_index = reader_->ReadUnsignedLEB128(*lineptr, &len);
          break;
        }
      }
      if (!AdvanceLinePtr(len, lineptr)) {
        return false;
      }
      const char* offset_ptr =
            get_offset_pointer_in_str_offsets_table(str_index,
                                                    str_offsets_buffer_,
                                                    str_offset_base_,
                                                    reader_->OffsetSize(),
                                                    header_.version);
      const uint64 offset = reader_->ReadOffset(offset_ptr);
      const char* string = get_string_with_offsets(str_buffer_, str_buffer_length_, offset);
      if(string)
        *dirname = string;
      else
        return false;
      break;
    }
    case DW_FORM_strp: {
      if(!str_buffer_ || !str_buffer_length_) 
        return false;
      const uint64 offset = reader_->ReadOffset(*lineptr);
      if (!AdvanceLinePtr(reader_->OffsetSize(), lineptr)) {
        return false;
      }
      const char* string = get_string_with_offsets(str_buffer_, str_buffer_length_, offset);
      if(string)
        *dirname = string;
      else
        return false;
      break;
    }
    default:
      return false;
  }

  return true;
}

bool LineInfo::ReadUnsignedForm(uint32 form, uint64* value,
    const char** lineptr) {
  size_t len;

  if (form == DW_FORM_udata) {
    *value = reader_->ReadUnsignedLEB128(*lineptr, &len);
    if (!AdvanceLinePtr(len, lineptr)) {
      return false;
    }
  } else if (form == DW_FORM_data1) {
    *value = reader_->ReadOneByte(*lineptr);
    if (!AdvanceLinePtr(1, lineptr)) {
      return false;
    }
  } else if (form == DW_FORM_data2) {
    *value = reader_->ReadTwoBytes(*lineptr);
    if (!AdvanceLinePtr(2, lineptr)) {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

// The header for a debug_line section is mildly complicated, because
// the line info is very tightly encoded.
void LineInfo::ReadHeader() {
  const char* lineptr = buffer_;
  size_t initial_length_size;
  const char* end_of_prologue_length;

  const uint64 initial_length = reader_->ReadInitialLength(lineptr, &initial_length_size);

  if (!AdvanceLinePtr(initial_length_size, &lineptr)) {
    return;
  }
  header_.unit_length = initial_length;
  if (buffer_ + initial_length_size + header_.unit_length
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
  if ((header_.version < 2 || header_.version > 5)
      && header_.version != VERSION_TWO_LEVEL) {
    malformed_ = true;
    return;
  }

  // Read DWARF5 Specific field
  if (header_.version == 5) {

      header_.address_size = reader_->ReadOneByte(lineptr);
      if (!AdvanceLinePtr(1, &lineptr)) {
        return;
      }

      header_.segment_selector_size = reader_->ReadOneByte (lineptr);
      if (!AdvanceLinePtr (1, &lineptr)) {
        return;
      }
  }

  header_.header_length = reader_->ReadOffset(lineptr);
  if (!AdvanceLinePtr(reader_->OffsetSize(), &lineptr)) {
    return;
  }

  end_of_prologue_length = lineptr;

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

  if (header_.version != VERSION_TWO_LEVEL 
      && header_.version != 5) {
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
    header_.logicals_offset = 0;
    header_.actuals_offset = 0;
  } 
  else if (header_.version == 5) {
    // Read the DWARF-5 directory table.
    {
      static const uint32 kMaxTypes = 4;
      uint32 content_types[kMaxTypes];
      uint32 content_forms[kMaxTypes];
      uint32 format_count;
      size_t len;

      if (!ReadTypesAndForms(&lineptr, content_types, content_forms,
          kMaxTypes, &format_count)) {
        malformed_ = true;
        return;
      }
      uint32 entry_count = reader_->ReadUnsignedLEB128(lineptr, &len);
      if (!AdvanceLinePtr(len, &lineptr)) {
        return;
      }
      for (uint32 row = 1; row <= entry_count; ++row) {
        const char* dirname = NULL;
        for (uint32 col = 0; col < format_count; ++col) {
          if (content_types[col] == DW_LNCT_path) {
            if (!ReadStringForm(content_forms[col], &dirname, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else {
            malformed_ = true;
            return;
          }
        }
        if (dirname == NULL) {
          malformed_ = true;
          return;
        }
        handler_->DefineDir(dirname, row);
      }
    }

    // Read the DWARF-5 filename table.
    {
      static const uint32 kMaxTypes = 4;
      uint32 content_types[kMaxTypes];
      uint32 content_forms[kMaxTypes];
      uint32 format_count;
      size_t len;

      if (!ReadTypesAndForms(&lineptr, content_types, content_forms,
          kMaxTypes, &format_count)) {
        malformed_ = true;
        return;
      }
      uint32 entry_count = reader_->ReadUnsignedLEB128(lineptr, &len);
      if (!AdvanceLinePtr(len, &lineptr)) {
        return;
      }
      for (uint32 row = 1; row <= entry_count; ++row) {
        const char* filename = NULL;
        uint64 dirindex = 0;
        uint64 mod_time = 0;
        uint64 filelength = 0;
        for (uint32 col = 0; col < format_count; ++col) {
          if (content_types[col] == DW_LNCT_path) {
            if (!ReadStringForm(content_forms[col], &filename, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else if (content_types[col] == DW_LNCT_directory_index) {
            if (!ReadUnsignedForm(content_forms[col], &dirindex, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else {
            malformed_ = true;
            return;
          }
        }
        if (filename == NULL) {
          malformed_ = true;
          return;
        }
        handler_->DefineFile(filename, row, dirindex, mod_time, filelength);
      }
    }
  }
  else {
    // Two-level line table.
    // Skip the fake directory and filename tables, and the fake
    // extended opcode that wraps the rest of the section.
    if (!AdvanceLinePtr(7, &lineptr)) {
      return;
    }

    // Logicals table offset.
    header_.logicals_offset = reader_->ReadOffset(lineptr);
    logicals_start_ = end_of_prologue_length + header_.logicals_offset;
    if (!AdvanceLinePtr(reader_->OffsetSize(), &lineptr)) {
      return;
    }

    // Actuals table offset.
    header_.actuals_offset = reader_->ReadOffset(lineptr);
    if (header_.actuals_offset > 0)
      actuals_start_ = end_of_prologue_length + header_.actuals_offset;
    if (!AdvanceLinePtr(reader_->OffsetSize(), &lineptr)) {
      return;
    }

    // Read the DWARF-5 directory table.
    {
      static const uint32 kMaxTypes = 4;
      uint32 content_types[kMaxTypes];
      uint32 content_forms[kMaxTypes];
      uint32 format_count;
      size_t len;

      if (!ReadTypesAndForms(&lineptr, content_types, content_forms,
          kMaxTypes, &format_count)) {
        malformed_ = true;
        return;
      }
      uint32 entry_count = reader_->ReadUnsignedLEB128(lineptr, &len);
      if (!AdvanceLinePtr(len, &lineptr)) {
        return;
      }
      for (uint32 row = 1; row <= entry_count; ++row) {
        const char* dirname = NULL;
        for (uint32 col = 0; col < format_count; ++col) {
          if (content_types[col] == DW_LNCT_path) {
            if (!ReadStringForm(content_forms[col], &dirname, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else {
            malformed_ = true;
            return;
          }
        }
        if (dirname == NULL) {
          malformed_ = true;
          return;
        }
        handler_->DefineDir(dirname, row);
      }
    }

    // Read the DWARF-5 filename table.
    {
      static const uint32 kMaxTypes = 4;
      uint32 content_types[kMaxTypes];
      uint32 content_forms[kMaxTypes];
      uint32 format_count;
      size_t len;

      if (!ReadTypesAndForms(&lineptr, content_types, content_forms,
          kMaxTypes, &format_count)) {
        malformed_ = true;
        return;
      }
      uint32 entry_count = reader_->ReadUnsignedLEB128(lineptr, &len);
      if (!AdvanceLinePtr(len, &lineptr)) {
        return;
      }
      for (uint32 row = 1; row <= entry_count; ++row) {
        const char* filename = NULL;
        uint64 dirindex = 0;
        uint64 mod_time = 0;
        uint64 filelength = 0;
        for (uint32 col = 0; col < format_count; ++col) {
          if (content_types[col] == DW_LNCT_path) {
            if (!ReadStringForm(content_forms[col], &filename, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else if (content_types[col] == DW_LNCT_directory_index) {
            if (!ReadUnsignedForm(content_forms[col], &dirindex, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else {
            malformed_ = true;
            return;
          }
        }
        if (filename == NULL) {
          malformed_ = true;
          return;
        }
        handler_->DefineFile(filename, row, dirindex, mod_time, filelength);
      }
    }

    // Read the subprogram table.
    {
      static const uint32 kMaxTypes = 4;
      uint32 content_types[kMaxTypes];
      uint32 content_forms[kMaxTypes];
      uint32 format_count;
      size_t len;

      if (!ReadTypesAndForms(&lineptr, content_types, content_forms,
          kMaxTypes, &format_count)) {
        malformed_ = true;
        return;
      }
      uint32 entry_count = reader_->ReadUnsignedLEB128(lineptr, &len);
      if (!AdvanceLinePtr(len, &lineptr)) {
        return;
      }
      for (uint32 row = 1; row <= entry_count; ++row) {
        const char* subprogname = NULL;
        uint64 decl_file = 0;
        uint64 decl_line = 0;
        for (uint32 col = 0; col < format_count; ++col) {
          if (content_types[col] == DW_LNCT_subprogram_name) {
            if (!ReadStringForm(content_forms[col], &subprogname, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else if (content_types[col] == DW_LNCT_decl_file) {
            if (!ReadUnsignedForm(content_forms[col], &decl_file, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else if (content_types[col] == DW_LNCT_decl_line) {
            if (!ReadUnsignedForm(content_forms[col], &decl_line, &lineptr)) {
              malformed_ = true;
              return;
            }
          } else {
            malformed_ = true;
            return;
          }
        }
        if (subprogname == NULL) {
          malformed_ = true;
          return;
        }
        handler_->DefineSubprog(subprogname, row, decl_file, decl_line);
      }
    }
  }

  after_header_ = lineptr;
}

/* static */
bool LineInfo::ProcessOneOpcode(
    ByteReader* reader,
    LineInfoHandler* handler,
    const struct LineInfoHeader &header,
    const char* start,
    struct LineStateMachine* lsm,
    size_t* len,
    const LogicalsVector *logicals,
    bool is_actuals) {
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
    case DW_LNS_set_subprogram:
      // This opcode is aliased with DW_LNS_set_address_from_logical.
      // One is used only in the logicals table, and the other is used
      // only in the actuals table.
      if (logicals != NULL && !is_actuals) {
        // We're reading the logicals table, so this is
        // DW_LNS_set_subprogram.
        const uint64 subprog_num = reader->ReadUnsignedLEB128(start, &templen);
        oplen += templen;
        lsm->subprog_num = subprog_num;
        lsm->context = 0;
      } else if (logicals != NULL && is_actuals) {
        // We're reading the actuals table, so this is
        // DW_LNS_set_address_from_logical.
        const int64 advance_line = reader->ReadSignedLEB128(start, &templen);
        oplen += templen;
        lsm->line_num += advance_line;
        if (lsm->line_num >= 1
            && lsm->line_num <= static_cast<int64>(logicals->size())) {
          const struct LineStateMachine& logical =
              (*logicals)[lsm->line_num - 1];
          lsm->address = logical.address;
        }
      } else {
        // Just skip the operand.
        reader->ReadSignedLEB128(start, &templen);
        oplen += templen;
        LOG(WARNING) << "DW_LNS_set_subprogram/set_address_from_logical "
            "opcode seen, but not in actuals table";
      }
      break;
    case DW_LNS_inlined_call: {
      const int64 advance_line = reader->ReadSignedLEB128(start, &templen);
      oplen += templen;
      start += templen;
      const int64 subprog_num = reader->ReadUnsignedLEB128(start, &templen);
      oplen += templen;
      if (logicals != NULL && !is_actuals) {
        lsm->context = logicals->size() + advance_line;
        lsm->subprog_num = subprog_num;
      } else {
        LOG(WARNING) << "DW_LNS_inlined_call opcode seen, "
                        "but not in actuals table";
      }
    }
      break;
    case DW_LNS_pop_context:
      if (logicals != NULL && !is_actuals) {
        if (lsm->context > 0 && lsm->context <= logicals->size()) {
          const struct LineStateMachine& logical =
              (*logicals)[lsm->context - 1];
          lsm->file_num = logical.file_num;
          lsm->line_num = logical.line_num;
          lsm->column_num = logical.column_num;
          lsm->discriminator = logical.discriminator;
          lsm->is_stmt = logical.is_stmt;
          lsm->context = logical.context;
          lsm->subprog_num = logical.subprog_num;
        }
      } else {
        LOG(WARNING) << "DW_LNS_pop_context opcode seen, "
                        "but not in actuals table";
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

  if (logicals_start_ != NULL && actuals_start_ != NULL) {
    // Two-level line table.
    LogicalsVector logicals;
    handler_->SetLogicals(&logicals);

    // Read the logicals table.
    const char* lineptr = logicals_start_;
    while (lineptr < actuals_start_) {
      lsm.Reset(header_.default_is_stmt);
      while (!lsm.end_sequence && lineptr < actuals_start_) {
        size_t oplength;
        bool add_line = ProcessOneOpcode(reader_, handler_, header_,
                                         lineptr, &lsm, &oplength,
                                         &logicals, false);
        if (add_line) {
          logicals.push_back(lsm);
          handler_->AddLogical(lsm.address, lsm.file_num, lsm.line_num,
                               lsm.column_num, lsm.discriminator,
                               lsm.context, lsm.subprog_num);
          lsm.basic_block = false;
          lsm.discriminator = 0;
        }
        lineptr += oplength;
      }
    }

    // Read the actuals table;
    handler_->StartActuals();
    lineptr = actuals_start_;
    while (lineptr < lengthstart + header_.unit_length) {
      lsm.Reset(header_.default_is_stmt);
      while (!lsm.end_sequence) {
        size_t oplength;
        bool add_line = ProcessOneOpcode(reader_, handler_, header_,
                                         lineptr, &lsm, &oplength,
                                         &logicals, true);
        if (add_line) {
          if (lsm.line_num >= 1
              && lsm.line_num <= static_cast<uint64>(logicals.size())) {
            const struct LineStateMachine& logical =
                logicals[lsm.line_num - 1];
            handler_->SetLogicalNum(lsm.line_num);
            handler_->SetContext(logical.context);
            handler_->SetSubprog(logical.subprog_num);
            handler_->AddLine(lsm.address, logical.file_num, logical.line_num,
                              logical.column_num, logical.discriminator,
                              lsm.end_sequence);
          }
          lsm.basic_block = false;
          lsm.discriminator = 0;
        }
        lineptr += oplength;
      }
    }

    handler_->SetLogicals(NULL);
  } else {
    // Normal line table.
    handler_->SetLogicals(NULL);
    handler_->SetContext(0);
    handler_->SetSubprog(0);
    const char* lineptr = after_header_;
    while (lineptr < lengthstart + header_.unit_length) {
      lsm.Reset(header_.default_is_stmt);
      while (!lsm.end_sequence) {
        size_t oplength;
        bool add_line = ProcessOneOpcode(reader_, handler_, header_,
                                         lineptr, &lsm, &oplength,
                                         NULL, false);
        if (add_line) {
          handler_->AddLine(lsm.address, lsm.file_num, lsm.line_num,
                            lsm.column_num, lsm.discriminator,
                            lsm.end_sequence);
          lsm.basic_block = false;
          lsm.discriminator = 0;
        }
        lineptr += oplength;
      }
    }
  }

  after_header_ = lengthstart + header_.unit_length;
}

}  // namespace devtools_crosstool_autofdo
