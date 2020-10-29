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
  LineIdentifier()
      : file(), line(0), discriminator(0), context(0), subprog_num(0) { }
  LineIdentifier(const DirectoryFilePair &file_in, uint32 line_in,
                 uint32 discriminator_in)
      : file(file_in), line(line_in), discriminator(discriminator_in),
        context(0), subprog_num(0) { }

  DirectoryFilePair file;
  uint32 line;
  uint32 discriminator;
  uint32 context;
  uint32 subprog_num;
};

// Address to line map to support two-level line tables.
// This class maps an address to a reference to a logical line
// table entry, which is represented by a LineIdentifier.
// An iterator it for this class contains a logical line
// table index as it->second, and the client can obtain the
// logical row via logical(it->second). If there is an
// inline call stack, the context field from LineIdentifier
// can be used to fetch the logical row for the calling
// context.
class AddressToLineMap {
 public:
  // A vector containing Subprogram entries.
  struct SubprogInfo {
    const char *name;
    DirectoryFilePair file;
    uint32 line_num;
  };
  typedef std::vector<struct SubprogInfo> SubprogVector;

  // Map an address to a logical row number.
  typedef std::map<uint64, uint32> AddressToLogical;
  typedef AddressToLogical::const_iterator const_iterator;

  AddressToLineMap()
    : subprogs_(), logical_lines_(), line_map_(), logical_map_(),
      subprog_bias_(0) { }

  void StartCU() {
    subprog_bias_ = subprogs_.size();
  }

  void AddSubprog(uint32 subprog_num, const char *name,
                  DirectoryFilePair file, uint32 line_num) {
    CHECK_EQ(subprog_bias_ + subprog_num - 1, subprogs_.size());
    SubprogInfo info = { name, file, line_num };
    subprogs_.push_back(info);
  }

  // Add a line from a normal line table (not two-level).
  // Adds both a logical entry and an actual entry.
  void AddLine(uint64 addr, LineIdentifier line_id) {
    logical_lines_.push_back(line_id);
    line_map_[addr] = logical_lines_.size();
  }

  // Resize the per-CU logical map to the size of the CU's
  // logicals table.
  void InitializeLogicalMap(uint32 num_logicals) {
    logical_map_.clear();
    // Logicals are 1-based.
    logical_map_.resize(num_logicals + 1);
  }

  // Map a per-CU logical line number to an index into the
  // logical_lines_ vector.
  uint32 MapLogical(int logical_num) {
    if (logical_num < 1 || logical_num >= logical_map_.size()) {
      LOG(INFO) << "error in MapLogical (bad logical_num "
                << logical_num << ")";
      return 0;
    }
    return logical_map_[logical_num];
  }

  // Add a logical line to the map.
  uint32 AddLogical(int logical_num, LineIdentifier line_id) {
    CHECK_GT(logical_num, 0);
    CHECK_LT(logical_num, logical_map_.size());
    if (logical_map_[logical_num] > 0) {
      return logical_map_[logical_num];
    }
    if (line_id.subprog_num > 0) {
      line_id.subprog_num += subprog_bias_;
    }
    logical_lines_.push_back(line_id);
    uint32 new_logical_num = logical_lines_.size();
    logical_map_[logical_num] = new_logical_num;
    return new_logical_num;
  }

  void AddActual(uint64 addr, uint64 logical_num) {
    line_map_[addr] = logical_num;
  }

  const_iterator begin() const {
    return line_map_.begin();
  }

  const_iterator end() const {
    return line_map_.end();
  }

  const_iterator upper_bound(uint64 addr) const {
    return line_map_.upper_bound(addr);
  }

  const LineIdentifier& GetLogical(uint32 logical_num) const {
    CHECK_GT(logical_num, 0);
    CHECK_LE(logical_num, logical_lines_.size());
    return logical_lines_[logical_num - 1];
  }

  const SubprogInfo& GetSubprogInfo(uint32 subprog_num) const {
    CHECK_GT(subprog_num, 0);
    CHECK_LE(subprog_num, subprogs_.size());
    return subprogs_[subprog_num - 1];
  }

 private:
  SubprogVector subprogs_;
  std::vector<LineIdentifier> logical_lines_;
  AddressToLogical line_map_;

  // The logical_lines_ vector stores only logicals that are actually
  // used by actuals that we keep (i.e., for non-deleted and/or
  // sampled functions). This maps a per-CU logical number onto the
  // index of that logical in logical_lines_, which accumulates
  // logicals across multiple CUs.
  std::vector<uint32> logical_map_;

  // This line map may accumulate entries from multiple CUs, so we need
  // to keep track of the first subprogram for the current CU, and adjust
  // all references into these arrays by this amount.
  uint32 subprog_bias_;
};

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

  // Called at the start of each CU.
  virtual void StartCU();

  // Called when we define a directory.  We just place NAME into dirs_
  // at position DIR_NUM.
  virtual void DefineDir(const char *name, uint32 dir_num);

  // Called when we define a filename.  We just place
  // concat(dirs_[DIR_NUM], NAME) into files_ at position FILE_NUM.
  virtual void DefineFile(const char *name, int32 file_num,
                          uint32 dir_num, uint64 mod_time, uint64 length);

  // Called when we define a subprogram.
  virtual void DefineSubprog(const char *name, uint32 subprog_num,
                             uint32 file_num, uint32 line_num);

  // Called when the logicals table has been read, and we are about
  // to start reading the actuals table.
  virtual void StartActuals();

  // Called when the line info reader has a new line, address pair
  // ready for us.  ADDRESS is the address of the code, FILE_NUM is
  // the file number containing the code, LINE_NUM is the line number
  // in that file for the code, and COLUMN_NUM is the column number
  // the code starts at, if we know it (0 otherwise).
  //
  // If this function is called more than once with the same address, the
  // information from the last call is stored.
  virtual void AddLine(uint64 address, uint32 file_num, uint32 line_num,
                       uint32 column_num, uint32 discriminator,
                       bool end_sequence);

  // Add a logical line and its inline stack to linemap2_.
  uint32 AddLogicalStack(uint32 logical_num);

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
