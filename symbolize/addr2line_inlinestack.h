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

#ifndef AUTOFDO_SYMBOLIZE_ADDR2LINE_INLINESTACK_H_
#define AUTOFDO_SYMBOLIZE_ADDR2LINE_INLINESTACK_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/common.h"
#include "symbolize/dwarf2enums.h"
#include "symbolize/dwarf2reader.h"
#include "symbolize/dwarf3ranges.h"
#include "symbolize/nonoverlapping_range_map.h"

namespace autofdo {

class ByteReader;

// This class tracks information about a DWARF subprogram as it
// pertains to computing line information for inlined functions.  In
// particular, for each subprogram (i.e., function) the class tracks
// whether the function is an inlined copy. If so, the class stores
// the offset in the debug section of the non-inlined copy of the
// function, into what function it is inlined (parent_), the line
// number of the call site, and the file name of the call site.  If it
// is not an inlined function, the function name is stored.  In both
// cases, the address ranges occupied by the Subprogram are stored.
class SubprogramInfo {
 public:
  SubprogramInfo(int cu_index, uint64 offset, const SubprogramInfo *parent,
                 bool inlined)
      : cu_index_(cu_index), offset_(offset), parent_(parent), name_(),
        inlined_(inlined), comp_directory_(NULL),
        callsite_directory_(NULL), callsite_filename_(NULL), callsite_line_(0),
        callsite_discr_(0), abstract_origin_(0), specification_(0),
        used_(false) { }

  const int cu_index() const { return cu_index_; }

  const uint64 offset() const { return offset_; }
  const SubprogramInfo *parent() const { return parent_; }

  void set_name(const char *name) { name_.assign(name); }
  const string& name() const { return name_; }

  // Address ranges are specified in the DWARF file as a reference to
  // a range list or a pair of DIEs specifying a single range.
  // SwapAddressRanges is used for the first case, while the
  // SetSingleton* methods are used for the latter.
  void SwapAddressRanges(AddressRangeList::RangeList *ranges);
  void SetSingletonRangeLow(uint64 addr);
  void SetSingletonRangeHigh(uint64 addr, bool is_offset);
  const AddressRangeList::RangeList *address_ranges() const {
    return &address_ranges_;
  }

  bool inlined() const { return inlined_; }

  void set_comp_directory(const char *dir) {
    comp_directory_ = dir;
  }

  const char *comp_directory() const {return comp_directory_;}

  void set_callsite_directory(const char *dir) {
    callsite_directory_ = dir;
  }
  const char *callsite_directory() const {
    return callsite_directory_;
  }

  void set_callsite_filename(const char *file) {
    callsite_filename_ = file;
  }
  const char *callsite_filename() const {
    return callsite_filename_;
  }

  // Returns a string representing the filename of this callsite.
  //
  // Args:
  //   basenames_only: just the filename
  //   with_comp_dir: prepend the compilation dir, if we have it.
  string CallsiteFilename(bool basenames_only, bool with_comp_dir) const;

  void set_callsite_line(uint32 line) { callsite_line_ = line; }
  uint32 callsite_line() const { return callsite_line_; }

  void set_callsite_discr(uint32 discr) { callsite_discr_ = discr; }
  uint32 callsite_discr() const { return callsite_discr_; }

  // The abstract origin refers to the details of an out-of-line or
  // inline concrete instance of an inline function.  See
  // http://www.dwarfstd.org for more details.
  void set_abstract_origin(uint64 offset) { abstract_origin_ = offset; }
  uint64 abstract_origin() const { return abstract_origin_; }

  void set_specification(uint64 offset) { specification_ = offset; }
  uint64 specification() const { return specification_; }

  void set_used() { used_ = true; }
  bool used() const { return used_; }

 private:
  int cu_index_;
  uint64 offset_;
  const SubprogramInfo *parent_;
  // The name may come from a .dwo file's string table, which will be
  // destroyed before we're done, so we need to make a copy.
  string name_;
  AddressRangeList::RangeList address_ranges_;
  bool inlined_;
  const char *comp_directory_;  // working dir of compilation.
  const char *callsite_directory_;
  const char *callsite_filename_;
  uint32 callsite_line_;
  uint32 callsite_discr_;
  uint64 abstract_origin_;
  uint64 specification_;
  bool used_;
  DISALLOW_COPY_AND_ASSIGN(SubprogramInfo);
};

// This class implements the callback interface used for reading DWARF
// debug information.  It stores information about all the observed
// subprograms.  The full set of subprograms can be retrieved, or the
// handler can be queried by address after all the debug information
// is read.  Between compilation units the set_filename method should
// be called to point to the correct filename information for the
// compilation unit.
class InlineStackHandler: public Dwarf2Handler {
 public:
  InlineStackHandler(
      AddressRangeList *address_ranges,
      const SectionMap& sections,
      ByteReader *reader,
      uint64 vaddr_of_first_load_segment)
      : InlineStackHandler(
          address_ranges, sections, reader, nullptr,
          vaddr_of_first_load_segment) {
  }

  InlineStackHandler(
      AddressRangeList *address_ranges,
      const SectionMap& sections,
      ByteReader *reader,
      const map<uint64, uint64> *sampled_functions,
      uint64 vaddr_of_first_load_segment)
      : directory_names_(NULL), file_names_(NULL), line_handler_(NULL),
        sections_(sections), reader_(reader),
        vaddr_of_first_load_segment_(vaddr_of_first_load_segment),
        address_ranges_(address_ranges),
        subprogram_stack_(), die_stack_(),
        cu_index_(-1), subprograms_by_offset_maps_(),
        compilation_unit_comp_dir_(), sampled_functions_(sampled_functions),
        overlap_count_(0)
  { }

  virtual bool StartCompilationUnit(uint64 offset, uint8 address_size,
                                    uint8 offset_size, uint64 cu_length,
                                    uint8 dwarf_version);

  virtual bool StartSplitCompilationUnit(uint64 offset, uint64 cu_length);

  virtual bool StartDIE(uint64 offset, enum DwarfTag tag,
                        const AttributeList& attrs);

  virtual void EndDIE(uint64 offset);

  virtual void ProcessAttributeString(uint64 offset,
                                      enum DwarfAttribute attr,
                                      enum DwarfForm form,
                                      const char *data);

  virtual void ProcessAttributeUnsigned(uint64 offset,
                                        enum DwarfAttribute attr,
                                        enum DwarfForm form,
                                        uint64 data);

  void set_directory_names(
      const DirectoryVector *directory_names) {
    directory_names_ = directory_names;
  }

  void set_file_names(const FileVector *file_names) {
    file_names_ = file_names;
  }

  void set_line_handler(LineInfoHandler *handler) {
    line_handler_ = handler;
  }

  const SubprogramInfo *GetSubprogramForAddress(uint64 address);

  const SubprogramInfo *GetDeclaration(const SubprogramInfo *subprog) const;

  const SubprogramInfo *GetAbstractOrigin(const SubprogramInfo *subprog) const;

  // Puts the start addresses of all inlined subprograms into the given set.
  void GetSubprogramAddresses(set<uint64> *addrs);

  // Cleans up memory consumed by subprograms that are not used.
  void CleanupUnusedSubprograms();

  void PopulateSubprogramsByAddress();

  ~InlineStackHandler();

 private:
  typedef map<uint64, SubprogramInfo*> SubprogramsByOffsetMap;

  void FindBadSubprograms(set<const SubprogramInfo *> *bad_subprograms);

  AddressRangeList::RangeList SortAndMerge(
      AddressRangeList::RangeList rangelist);

  const DirectoryVector *directory_names_;
  const FileVector *file_names_;
  LineInfoHandler *line_handler_;
  const SectionMap& sections_;
  ByteReader *reader_;
  uint64 vaddr_of_first_load_segment_;
  AddressRangeList *address_ranges_;
  vector<SubprogramInfo*> subprogram_stack_;
  vector<DwarfTag> die_stack_;
  // We keep a separate map from offset to SubprogramInfo for each CU,
  // because when reading .dwo or .dwp files, the offsets are relative
  // to the beginning of the debug info for that CU.
  int cu_index_;
  vector<SubprogramsByOffsetMap*> subprograms_by_offset_maps_;
  vector<SubprogramInfo *> subprogram_insert_order_;
  NonOverlappingRangeMap<SubprogramInfo*> subprograms_by_address_;
  uint64 compilation_unit_offset_;
  uint64 compilation_unit_base_;
  // The comp dir name may come from a .dwo file's string table, which
  // will be destroyed before we're done, so we need to copy it for
  // each compilation unit.  We need to keep a vector of all the
  // directories that we've seen, because SubprogramInfo keeps
  // StringPiece objects pointing to these copies.
  vector<string*> compilation_unit_comp_dir_;
  const map<uint64, uint64> *sampled_functions_;
  int overlap_count_;
  DISALLOW_COPY_AND_ASSIGN(InlineStackHandler);
};

}  // namespace autofdo

#endif  // AUTOFDO_SYMBOLIZE_ADDR2LINE_INLINESTACK_H_
