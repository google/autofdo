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

// This file contains the definitions for a DWARF2/3 information
// collector that uses the DWARF2/3 reader interface to build a mapping
// of addresses to files, lines, and functions.
// This is much more of an example of using the readers than anything
// else, the linemap would use too much memory for a real program.

#ifndef AUTOFDO_SYMBOLIZE_FUNCTIONINFO_H__
#define AUTOFDO_SYMBOLIZE_FUNCTIONINFO_H__

#include <map>
#include <string>
#include <utility>

#include "base/common.h"
#include "symbolize/bytereader.h"
#include "symbolize/dwarf2reader.h"

namespace autofdo {

typedef pair<const char *, const char *> DirectoryFilePair;

struct FunctionInfo {
  // Name of the function
  const char *name;
  // File containing this function
  DirectoryFilePair file;
  // Line number for start of function.
  uint32 line;
  // Beginning address for this function
  uint64 lowpc;
  // End address for this function.
  uint64 highpc;
};

typedef map<uint64, FunctionInfo*> FunctionMap;

struct LineIdentifier {
  LineIdentifier() : file(), line(), discriminator() { }
  LineIdentifier(const DirectoryFilePair &file_in, uint32 line_in,
                 uint32 discriminator_in)
      : file(file_in), line(line_in), discriminator(discriminator_in) { }

  DirectoryFilePair file;
  uint32 line;
  uint32 discriminator;
};

typedef map<uint64, LineIdentifier> AddressToLineMap;

static int strcmp_maybe_null(const char *a, const char *b) {
  if (a == 0 && b == 0) {
    return 0;
  } else if (a == 0) {
    return -1;
  } else if (b == 0) {
    return 1;
  } else {
    return strcmp(a, b);
  }
}

// Compares two LineIdentifiers.  This is useful when using
// LineIdentifiers as the key of an STL map.  The comparison is
// lexicographic order first based on directory name, then filename,
// and finally line number.
//
// Note, this comparator treats the following cases differently:
//
// (("/", "foo/bar.cc"), 1)
// (("/foo", "bar.cc"), 1)
// (("/foo/baz", "../bar.cc"), 1)
//
// While all three point to the same file and line number, they will
// not be considered equal by the comparator.  The order shown is the
// order returned by the comparator.  However, since these patterns
// will not happen in google3 code, it should not matter.
struct LessThanLineIdentifier {
  bool operator()(const LineIdentifier &line_a,
                  const LineIdentifier &line_b) const {
    int cmp = strcmp_maybe_null(line_a.file.first, line_b.file.first);
    if (cmp == 0) {
      cmp = strcmp_maybe_null(line_a.file.second, line_b.file.second);
      if (cmp == 0) {
        cmp = (line_a.line - line_b.line);
        if (cmp == 0)
          cmp = (line_a.discriminator - line_b.discriminator);
      }
    }
    return (cmp < 0);
  }
};

// This class is a basic line info handler that fills in the dirs,
// file, and linemap passed into it with the data produced from the
// LineInfoHandler.
class CULineInfoHandler: public LineInfoHandler {
 public:
  CULineInfoHandler(FileVector* files,
                    DirectoryVector* dirs,
                    AddressToLineMap* linemap);
  CULineInfoHandler(FileVector* files,
                    DirectoryVector* dirs,
                    AddressToLineMap* linemap,
                    const map<uint64, uint64> *sampled_functions);
  virtual ~CULineInfoHandler() { }

  // Called when we define a directory.  We just place NAME into dirs_
  // at position DIR_NUM.
  virtual void DefineDir(const char *name, uint32 dir_num);

  // Called when we define a filename.  We just place
  // concat(dirs_[DIR_NUM], NAME) into files_ at position FILE_NUM.
  virtual void DefineFile(const char *name, int32 file_num,
                          uint32 dir_num, uint64 mod_time, uint64 length);


  // Called when the line info reader has a new line, address pair
  // ready for us.  ADDRESS is the address of the code, FILE_NUM is
  // the file number containing the code, LINE_NUM is the line number
  // in that file for the code, and COLUMN_NUM is the column number
  // the code starts at, if we know it (0 otherwise).
  //
  // If this function is called more than once with the same address, the
  // information from the last call is stored.
  virtual void AddLine(uint64 address, uint32 file_num, uint32 line_num,
                       uint32 column_num, uint32 discriminator);


  static string MergedFilename(const DirectoryFilePair& filename);

 private:
  void Init();
  // Returns true if address should be added to linemap_.
  bool ShouldAddAddress(uint64 address) const;

  AddressToLineMap* linemap_;
  FileVector* files_;
  DirectoryVector* dirs_;
  const map<uint64, uint64> *sampled_functions_;
  DISALLOW_EVIL_CONSTRUCTORS(CULineInfoHandler);
};

class CUFunctionInfoHandler: public Dwarf2Handler {
 public:
  CUFunctionInfoHandler(FileVector* files,
                        DirectoryVector* dirs,
                        AddressToLineMap* linemap,
                        FunctionMap* offset_to_funcinfo,
                        FunctionMap* address_to_funcinfo,
                        CULineInfoHandler* linehandler,
                        const SectionMap& sections,
                        ByteReader* reader)
      : files_(files), dirs_(dirs), linemap_(linemap),
        offset_to_funcinfo_(offset_to_funcinfo),
        address_to_funcinfo_(address_to_funcinfo),
        linehandler_(linehandler), sections_(sections),
        reader_(reader), current_function_info_(NULL) { }

  virtual ~CUFunctionInfoHandler() { }

  // Start to process a compilation unit at OFFSET from the beginning of the
  // debug_info section.  We want to see all compilation units, so we
  // always return true.

  virtual bool StartCompilationUnit(uint64 offset, uint8 address_size,
                                    uint8 offset_size, uint64 cu_length,
                                    uint8 dwarf_version);

  // Start to process a DIE at OFFSET from the beginning of the
  // debug_info section.  We only care about function related DIE's.
  virtual bool StartDIE(uint64 offset, enum DwarfTag tag,
                        const AttributeList& attrs);

  // Called when we have an attribute with unsigned data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA.
  virtual void ProcessAttributeUnsigned(uint64 offset,
                                        enum DwarfAttribute attr,
                                        enum DwarfForm form,
                                        uint64 data);

  // Called when we have an attribute with string data to give to
  // our handler.  The attribute is for the DIE at OFFSET from the
  // beginning of compilation unit, has a name of ATTR, a form of
  // FORM, and the actual data of the attribute is in DATA.
  virtual void ProcessAttributeString(uint64 offset,
                                      enum DwarfAttribute attr,
                                      enum DwarfForm form,
                                      const char *data);

  // Called when finished processing the DIE at OFFSET.
  // Because DWARF2/3 specifies a tree of DIEs, you may get starts
  // before ends of the previous DIE, as we process children before
  // ending the parent.
  virtual void EndDIE(uint64 offset);

 private:
  FileVector* files_;
  DirectoryVector* dirs_;
  AddressToLineMap* linemap_;
  FunctionMap* offset_to_funcinfo_;
  FunctionMap* address_to_funcinfo_;
  CULineInfoHandler* linehandler_;
  const SectionMap& sections_;
  ByteReader* reader_;
  FunctionInfo* current_function_info_;
  DISALLOW_EVIL_CONSTRUCTORS(CUFunctionInfoHandler);
};

}  // namespace autofdo
#endif  // AUTOFDO_SYMBOLIZE_FUNCTIONINFO_H__
