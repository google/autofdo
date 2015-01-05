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

// This file contains definitions related to the DWARF2/3 reader and
// it's handler interfaces.
// The DWARF2/3 specification can be found at
// http://dwarf.freestandards.org and should be considered required
// reading if you wish to modify the implementation.
// I have only made a cursory attempt to explain terminology that is
// used here, as it is much better explained in the standard documents
#ifndef AUTOFDO_SYMBOLIZE_DWARF2READER_H__
#define AUTOFDO_SYMBOLIZE_DWARF2READER_H__

#include <stddef.h>
#include <stdint.h>
#include <map>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include "base/common.h"
#include "symbolize/dwarf2enums.h"

namespace autofdo {
class ElfReader;
class ByteReader;
class Dwarf2Handler;
class LineInfoHandler;
class DwpReader;

// This maps from a string naming a section to a pair containing a
// the data for the section, and the size of the section.
typedef map<string, pair<const char*, uint64> > SectionMap;
typedef list<pair<enum DwarfAttribute, enum DwarfForm> > AttributeList;
typedef AttributeList::iterator AttributeIterator;
typedef AttributeList::const_iterator ConstAttributeIterator;

// A vector containing directory names
typedef vector<const char *> DirectoryVector;
// A vector containing a directory name index and a file name
typedef vector<pair<int, const char *> > FileVector;

struct LineInfoHeader {
  uint64 total_length;
  uint16 version;
  uint64 prologue_length;
  uint8 min_insn_length;  // insn stands for instruction
  uint8 max_ops_per_insn;
  bool default_is_stmt;  // stmt stands for statement
  int8 line_base;
  uint8 line_range;
  uint8 opcode_base;
  // Use a pointer so that signalsafe_addr2line is able to use this structure
  // without heap allocation problem.
  vector<unsigned char> *std_opcode_lengths;
};

class LineInfo {
 public:
  // Initializes a .debug_line reader. Buffer and buffer length point
  // to the beginning and length of the line information to read.
  // Reader is a ByteReader class that has the endianness set
  // properly.
  LineInfo(const char* buffer_, uint64 buffer_length,
           ByteReader* reader, LineInfoHandler* handler);

  virtual ~LineInfo() {
    if (header_.std_opcode_lengths) {
      delete header_.std_opcode_lengths;
    }
  }

  bool malformed() const {return malformed_;}

  // Start processing line info, and calling callbacks in the handler.
  // Consumes the line number information for a single compilation unit.
  // Returns the number of bytes processed.
  uint64 Start();

  // Process a single line info opcode at START using the state
  // machine at LSM.  Return true if we should define a line using the
  // current state of the line state machine.  Place the length of the
  // opcode in LEN.
  static bool ProcessOneOpcode(ByteReader* reader,
                               LineInfoHandler* handler,
                               const struct LineInfoHeader &header,
                               const char* start,
                               struct LineStateMachine* lsm,
                               size_t* len,
                               uintptr_t pc);

 private:
  // Advance lineptr in a buffer.  If lineptr will advance beyond the
  // end of buffer_, malformed_ is set true.
  //
  // Arguments:
  //  incr: how far to increment
  //  lineptr: pointer to adjust
  //
  // Returns true if lineptr is advanced.
  bool AdvanceLinePtr(int incr,  const char **lineptr);

  // Reads the DWARF2/3 header for this line info.
  void ReadHeader();

  // Reads the DWARF2/3 line information
  void ReadLines();

  // The associated handler to call processing functions in
  LineInfoHandler* handler_;

  // The associated ByteReader that handles endianness issues for us
  ByteReader* reader_;

  // A DWARF2/3 line info header.  This is not the same size as
  // in the actual file, as the one in the file may have a 32 bit or
  // 64 bit lengths

  struct LineInfoHeader header_;

  // buffer is the buffer for our line info, starting at exactly where
  // the line info to read is.  after_header is the place right after
  // the end of the line information header.
  const char* buffer_;
  uint64 buffer_length_;
  const char* after_header_;
  bool malformed_;
  DISALLOW_EVIL_CONSTRUCTORS(LineInfo);
};

// This class is the main interface between the line info reader and
// the client.  The virtual functions inside this get called for
// interesting events that happen during line info reading.  The
// default implementation does nothing

class LineInfoHandler {
 public:
  LineInfoHandler() { }

  virtual ~LineInfoHandler() { }

  // Called when we define a directory.  NAME is the directory name,
  // DIR_NUM is the directory number
  virtual void DefineDir(const char *name, uint32 dir_num) { }

  // Called when we define a filename. NAME is the filename, FILE_NUM
  // is the file number which is -1 if the file index is the next
  // index after the last numbered index (this happens when files are
  // dynamically defined by the line program), DIR_NUM is the
  // directory index for the directory name of this file, MOD_TIME is
  // the modification time of the file, and LENGTH is the length of
  // the file
  virtual void DefineFile(const char *name, int32 file_num,
                          uint32 dir_num, uint64 mod_time,
                          uint64 length) { }

  // Called when the line info reader has a new line, address pair
  // ready for us.  ADDRESS is the address of the code, FILE_NUM is
  // the file number containing the code, LINE_NUM is the line number
  // in that file for the code, COLUMN_NUM is the column number the
  // code starts at, if we know it (0 otherwise), and DISCRIMINATOR is
  // the path discriminator identifying the basic block on the
  // specified line.
  virtual void AddLine(uint64 address, uint32 file_num, uint32 line_num,
                       uint32 column_num, uint32 discriminator) { }

 private:
  DISALLOW_EVIL_CONSTRUCTORS(LineInfoHandler);
};

// This class is the main interface between the reader and the
// client.  The virtual functions inside this get called for
// interesting events that happen during DWARF2 reading.
// The default implementation skips everything.

class Dwarf2Handler {
 public:
  Dwarf2Handler() { }

  virtual ~Dwarf2Handler() { }

  // Start to process a compilation unit at OFFSET from the beginning of the
  // debug_info section.  Return false if you would like
  // to skip this compilation unit.
  virtual bool StartCompilationUnit(uint64 offset, uint8 address_size,
                                    uint8 offset_size, uint64 cu_length,
                                    uint8 dwarf_version) { return false; }

  // Start to process a split compilation unit at OFFSET from the beginning of
  // the debug_info section in the .dwp/.dwo file.  Return false if you would
  // like to skip this compilation unit.
  virtual bool StartSplitCompilationUnit(uint64 offset,
                                         uint64 cu_length) { return false; }

  // Start to process a DIE at OFFSET from the beginning of the
  // debug_info section.  Return false if you would like to skip this
  // DIE.
  virtual bool StartDIE(uint64 offset, enum DwarfTag tag,
                        const AttributeList& attrs) { return false; }

  // Called when we have an attribute with unsigned data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA.
  virtual void ProcessAttributeUnsigned(uint64 offset,
                                        enum DwarfAttribute attr,
                                        enum DwarfForm form,
                                        uint64 data) { }

  // Called when we have an attribute with signed data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA.
  virtual void ProcessAttributeSigned(uint64 offset,
                                      enum DwarfAttribute attr,
                                      enum DwarfForm form,
                                      int64 data) { }

  // Called when we have an attribute with a buffer of data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA, and the
  // length of the buffer is LENGTH.  The buffer is owned by the
  // caller, not the callee, and may not persist for very long.  If
  // you want the data to be available later, it needs to be copied.
  virtual void ProcessAttributeBuffer(uint64 offset,
                                      enum DwarfAttribute attr,
                                      enum DwarfForm form,
                                      const char* data,
                                      uint64 len) { }

  // Called when we have an attribute with string data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA.
  virtual void ProcessAttributeString(uint64 offset,
                                      enum DwarfAttribute attr,
                                      enum DwarfForm form,
                                      const char* data) { }

  // Called when finished processing the DIE at OFFSET.
  // Because DWARF2/3 specifies a tree of DIEs, you may get starts
  // before ends of the previous DIE, as we process children before
  // ending the parent.
  virtual void EndDIE(uint64 offset) { }

 private:
  DISALLOW_EVIL_CONSTRUCTORS(Dwarf2Handler);
};

// The base of DWARF2/3 debug info is a DIE (Debugging Information
// Entry.
// DWARF groups DIE's into a tree and calls the root of this tree a
// "compilation unit".  Most of the time, their is one compilation
// unit in the .debug_info section for each file that had debug info
// generated.
// Each DIE consists of

// 1. a tag specifying a thing that is being described (ie
// DW_TAG_subprogram for functions, DW_TAG_variable for variables, etc
// 2. attributes (such as DW_AT_location for location in memory,
// DW_AT_name for name), and data for each attribute.
// 3. A flag saying whether the DIE has children or not

// In order to gain some amount of compression, the format of
// each DIE (tag name, attributes and data forms for the attributes)
// are stored in a separate table called the "abbreviation table".
// This is done because a large number of DIEs have the exact same tag
// and list of attributes, but different data for those attributes.
// As a result, the .debug_info section is just a stream of data, and
// requires reading of the .debug_abbrev section to say what the data
// means.

// As a warning to the user, it should be noted that the reason for
// using absolute offsets from the beginning of .debug_info is that
// DWARF2/3 support referencing DIE's from other DIE's by their offset
// from either the current compilation unit start, *or* the beginning
// of the .debug_info section.  This means it is possible to reference
// a DIE in one compilation unit from a DIE in another compilation
// unit.  This style of reference is usually used to eliminate
// duplicated information that occurs across compilation
// units, such as base types, etc.  GCC 3.4+ support this with
// -feliminate-dwarf2-dups.  Other toolchains will sometimes do
// duplicate elimination in the linker.

class CompilationUnit {
 public:
  // Initialize a compilation unit.  This requires a map of sections,
  // the offset of this compilation unit in the debug_info section, a
  // ByteReader, and a Dwarf2Handler class to call callbacks in.
  CompilationUnit(const string& path, const SectionMap& sections,
                  uint64 offset, ByteReader* reader, Dwarf2Handler* handler);

  virtual ~CompilationUnit();

  // Initialize a compilation unit from a .dwo or .dwp file.
  // In this case, we need the .debug_addr section from the
  // executable file that contains the corresponding skeleton
  // compilation unit.  We also inherit the Dwarf2Handler from
  // the executable file, and call it as if we were still
  // processing the original compilation unit.
  void SetSplitDwarf(const char* addr_buffer, uint64 addr_buffer_length,
                     uint64 addr_base, uint64 ranges_base);

  bool malformed() const {return malformed_;}

  // Begin reading a Dwarf2 compilation unit, and calling the
  // callbacks in the Dwarf2Handler
  // Return the offset of the end of the compilation unit - the passed
  // in offset.
  uint64 Start();

  // Begin reading a Dwarf2 compilation unit at the offset specified.
  // Return the offset of the end of the compilation unit - the passed
  // in offset.
  uint64 Start(uint64 offset);

 private:
  // This struct represents a single DWARF2/3 abbreviation
  // The abbreviation tells how to read a DWARF2/3 DIE, and consist of a
  // tag and a list of attributes, as well as the data form of each attribute.
  struct Abbrev {
    uint32 number;
    enum DwarfTag tag;
    bool has_children;
    AttributeList attributes;
  };

  // A DWARF2/3 compilation unit header.  This is not the same size as
  // in the actual file, as the one in the file may have a 32 bit or
  // 64 bit length.
  struct CompilationUnitHeader {
    uint64 length;
    uint16 version;
    uint64 abbrev_offset;
    uint8 address_size;
  } header_;

  // Reads the DWARF2/3 header for this compilation unit.
  void ReadHeader();

  // Reads the DWARF2/3 abbreviations for this compilation unit
  void ReadAbbrevs();

  // Processes a single DIE for this compilation unit.
  //
  // Returns a new pointer just past the end of it, or NULL if a
  // malformed DIE is encountered.
  const char* ProcessDIE(uint64 dieoffset,
                                  const char* start,
                                  const Abbrev& abbrev);

  // Processes a single attribute.
  //
  // Returns a new pointer just past the end of it, or NULL if malformed.
  const char* ProcessAttribute(uint64 dieoffset,
                               const char* start,
                               enum DwarfAttribute attr,
                               enum DwarfForm form);

  // Called when we have an attribute with unsigned data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA.
  // If we see a DW_AT_GNU_dwo_id attribute, save the value so that
  // we can find the debug info in a .dwo or .dwp file.
  void ProcessAttributeUnsigned(uint64 offset,
                                enum DwarfAttribute attr,
                                enum DwarfForm form,
                                uint64 data) {
    if (attr == DW_AT_GNU_dwo_id)
      dwo_id_ = data;
    else if (attr == DW_AT_GNU_addr_base)
      addr_base_ = data;
    else if (attr == DW_AT_GNU_ranges_base)
      ranges_base_ = data;
    else if (attr == DW_AT_ranges)
      data += ranges_base_;
    handler_->ProcessAttributeUnsigned(offset, attr, form, data);
  }

  // Called when we have an attribute with signed data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA.
  void ProcessAttributeSigned(uint64 offset,
                              enum DwarfAttribute attr,
                              enum DwarfForm form,
                              int64 data) {
    handler_->ProcessAttributeSigned(offset, attr, form, data);
  }

  // Called when we have an attribute with a buffer of data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA, and the
  // length of the buffer is LENGTH.
  void ProcessAttributeBuffer(uint64 offset,
                              enum DwarfAttribute attr,
                              enum DwarfForm form,
                              const char* data,
                              uint64 len) {
    handler_->ProcessAttributeBuffer(offset, attr, form, data, len);
  }

  // Called when we have an attribute with string data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA.
  // If we see a DW_AT_GNU_dwo_name attribute, save the value so
  // that we can find the debug info in a .dwo or .dwp file.
  void ProcessAttributeString(uint64 offset,
                              enum DwarfAttribute attr,
                              enum DwarfForm form,
                              const char* data) {
    if (attr == DW_AT_GNU_dwo_name)
      dwo_name_ = data;
    handler_->ProcessAttributeString(offset, attr, form, data);
  }

  // Processes all DIEs for this compilation unit
  void ProcessDIEs();

  // Skips the die with attributes specified in ABBREV starting at
  // START, and return the new place to position the stream to.
  const char* SkipDIE(const char* start, const Abbrev& abbrev);

  // Skips the attribute starting at START, with FORM, and return the
  // new place to position the stream to.
  const char* SkipAttribute(const char* start, enum DwarfForm form);

  // Process the actual debug information in a split DWARF file.
  void ProcessSplitDwarf();

  // Read the debug sections from a .dwo file.
  void ReadDebugSectionsFromDwo(ElfReader* elf_reader,
                                SectionMap* sections);

  // Path of the file containing the debug information.
  const string path_;

  // Offset from section start is the offset of this compilation unit
  // from the beginning of the .debug_info section.
  uint64 offset_from_section_start_;

  // buffer is the buffer for our CU, starting at .debug_info + offset
  // passed in from constructor.
  // after_header points to right after the compilation unit header.
  const char* buffer_;
  uint64 buffer_length_;
  const char* after_header_;

  // The associated ByteReader that handles endianness issues for us
  ByteReader* reader_;

  // The map of sections in our file to buffers containing their data
  const SectionMap& sections_;

  // The associated handler to call processing functions in
  Dwarf2Handler* handler_;

  // Set of DWARF2/3 abbreviations for this compilation unit.  Indexed
  // by abbreviation number, which means that abbrevs_[0] is not
  // valid.
  vector<Abbrev>* abbrevs_;

  // String section buffer and length, if we have a string section.
  // This is here to avoid doing a section lookup for strings in
  // ProcessAttribute, which is in the hot path for DWARF2 reading.
  const char* string_buffer_;
  uint64 string_buffer_length_;

  // String offsets section buffer and length, if we have a string offsets
  // section (.debug_str_offsets or .debug_str_offsets.dwo).
  const char* str_offsets_buffer_;
  uint64 str_offsets_buffer_length_;

  // Address section buffer and length, if we have an address section
  // (.debug_addr).
  const char* addr_buffer_;
  uint64 addr_buffer_length_;

  // Flag indicating whether this compilation unit is part of a .dwo
  // or .dwp file.  If true, we are reading this unit because a
  // skeleton compilation unit in an executable file had a
  // DW_AT_GNU_dwo_name or DW_AT_GNU_dwo_id attribute.
  // In a .dwo file, we expect the string offsets section to
  // have a ".dwo" suffix, and we will use the ".debug_addr" section
  // associated with the skeleton compilation unit.
  bool is_split_dwarf_;

  // The value of the DW_AT_GNU_dwo_id attribute, if any.
  uint64 dwo_id_;

  // The value of the DW_AT_GNU_dwo_name attribute, if any.
  const char* dwo_name_;

  // The value of the DW_AT_GNU_ranges_base attribute, if any.
  uint64 ranges_base_;

  // The value of the DW_AT_GNU_addr_base attribute, if any.
  uint64 addr_base_;

  // True if we have already looked for a .dwp file.
  bool have_checked_for_dwp_;

  // Path to the .dwp file.
  string dwp_path_;

  // ByteReader for the DWP file.
  ByteReader* dwp_byte_reader_;

  // DWP reader.
  DwpReader* dwp_reader_;

  bool malformed_;
  DISALLOW_EVIL_CONSTRUCTORS(CompilationUnit);
};

// A Reader for a .dwp file.  Supports the fetching of DWARF debug
// info for a given dwo_id.
//
// There are two versions of .dwp files.  In both versions, the
// .dwp file is an ELF file containing only debug sections.
// In Version 1, the file contains many copies of each debug
// section, one for each .dwo file that is packaged in the .dwp
// file, and the .debug_cu_index section maps from the dwo_id
// to a set of section indexes.  In Version 2, the file contains
// one of each debug section, and the .debug_cu_index section
// maps from the dwo_id to a set of offsets and lengths that
// identify each .dwo file's contribution to the larger sections.

class DwpReader {
 public:
  DwpReader(const ByteReader& byte_reader, ElfReader* elf_reader);

  ~DwpReader();

  // Read the CU index and initialize data members.
  void Initialize();

  // Read the debug sections for the given dwo_id.
  void ReadDebugSectionsForCU(uint64 dwo_id, SectionMap* sections);

 private:
  // Search a v1 hash table for "dwo_id".  Returns the slot index
  // where the dwo_id was found, or -1 if it was not found.
  int LookupCU(uint64 dwo_id);

  // Search a v2 hash table for "dwo_id".  Returns the row index
  // in the offsets and sizes tables, or 0 if it was not found.
  uint32 LookupCUv2(uint64 dwo_id);

  // The ELF reader for the .dwp file.
  ElfReader* elf_reader_;

  // The ByteReader for the .dwp file.
  const ByteReader& byte_reader_;

  // Pointer to the .debug_cu_index section.
  const char* cu_index_;

  // Size of the .debug_cu_index section.
  size_t cu_index_size_;

  // Pointer to the .debug_str.dwo section.
  const char* string_buffer_;

  // Size of the .debug_str.dwo section.
  size_t string_buffer_size_;

  // Version of the .dwp file.  We support versions 1 and 2 currently.
  int version_;

  // Number of columns in the section tables (version 2).
  unsigned int ncolumns_;

  // Number of units in the section tables (version 2).
  unsigned int nunits_;

  // Number of slots in the hash table.
  unsigned int nslots_;

  // Pointer to the beginning of the hash table.
  const char* phash_;

  // Pointer to the beginning of the index table.
  const char* pindex_;

  // Pointer to the beginning of the section index pool (version 1).
  const char* shndx_pool_;

  // Pointer to the beginning of the section offset table (version 2).
  const char* offset_table_;

  // Pointer to the beginning of the section size table (version 2).
  const char* size_table_;

  // Contents of the sections of interest (version 2).
  const char* abbrev_data_;
  size_t abbrev_size_;
  const char* info_data_;
  size_t info_size_;
  const char* str_offsets_data_;
  size_t str_offsets_size_;
};

}  // namespace autofdo

#endif  // AUTOFDO_SYMBOLIZE_DWARF2READER_H__
