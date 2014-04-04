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

#include "symbolize/addr2line_inlinestack.h"

#include <utility>

#include "base/logging.h"
#include "symbolize/bytereader.h"

namespace autofdo {

void SubprogramInfo::SwapAddressRanges(AddressRangeList::RangeList *ranges) {
  address_ranges_.swap(*ranges);
}

void SubprogramInfo::SetSingletonRangeLow(uint64 addr) {
  if (address_ranges_.empty()) {
    address_ranges_.push_back(make_pair(addr, 0ULL));
  } else {
    CHECK_EQ(1, address_ranges_.size());
    address_ranges_[0].first = addr;
  }
}

void SubprogramInfo::SetSingletonRangeHigh(uint64 addr, bool is_offset) {
  if (address_ranges_.empty()) {
    address_ranges_.push_back(make_pair(0ULL, addr));
  } else {
    CHECK_EQ(1, address_ranges_.size());
    if (is_offset)
      address_ranges_[0].second = address_ranges_[0].first + addr;
    else
      address_ranges_[0].second = addr;
  }
}

bool InlineStackHandler::StartCompilationUnit(uint64 offset,
                                              uint8 /*address_size*/,
                                              uint8 /*offset_size*/,
                                              uint64 /*cu_length*/,
                                              uint8 /*dwarf_version*/) {
  CHECK(subprogram_stack_.empty());
  compilation_unit_offset_ = offset;
  compilation_unit_base_ = 0;
  ++cu_index_;
  subprograms_by_offset_maps_.push_back(new SubprogramsByOffsetMap);
  CHECK(subprograms_by_offset_maps_.size() == cu_index_ + 1);
  return true;
}

bool InlineStackHandler::StartDIE(uint64 offset,
                                  enum DwarfTag tag,
                                  const AttributeList& attrs) {
  die_stack_.push_back(tag);

  switch (tag) {
    case DW_TAG_subprogram:
    case DW_TAG_inlined_subroutine: {
      bool inlined = (tag == DW_TAG_inlined_subroutine);
      SubprogramInfo *parent =
          subprogram_stack_.empty() ? NULL : subprogram_stack_.back();
      SubprogramInfo *child =
          new SubprogramInfo(cu_index_, offset, parent, inlined);
      if (!compilation_unit_comp_dir_.empty())
        child->set_comp_directory(compilation_unit_comp_dir_.back()->c_str());
      SubprogramsByOffsetMap* subprograms_by_offset =
          subprograms_by_offset_maps_.back();
      subprograms_by_offset->insert(make_pair(offset, child));
      subprogram_stack_.push_back(child);
      return true;
    }
    case DW_TAG_compile_unit:
      return true;
    default:
      return false;
  }
}

void InlineStackHandler::EndDIE(uint64 offset) {
  DwarfTag die = die_stack_.back();
  die_stack_.pop_back();
  if (die == DW_TAG_subprogram ||
      die == DW_TAG_inlined_subroutine) {
    subprogram_insert_order_.push_back(subprogram_stack_.back());
    subprogram_stack_.pop_back();
  }
}

void InlineStackHandler::ProcessAttributeString(
    uint64 offset,
    enum DwarfAttribute attr,
    enum DwarfForm form,
    const char *data) {
  if (attr == DW_AT_comp_dir) {
    compilation_unit_comp_dir_.emplace_back(new string(data));
  }

  if (!subprogram_stack_.empty()) {
    // Use the mangled name if it exists, otherwise use the demangled name
    if (attr == DW_AT_MIPS_linkage_name
        || attr == DW_AT_linkage_name) {
      subprogram_stack_.back()->set_name(data);
    } else if (attr == DW_AT_name &&
               subprogram_stack_.back()->name().empty()) {
      subprogram_stack_.back()->set_name(data);
    }
  }
}

void InlineStackHandler::ProcessAttributeUnsigned(
    uint64 offset,
    enum DwarfAttribute attr,
    enum DwarfForm form,
    uint64 data) {
  if (!subprogram_stack_.empty()) {
    switch (attr) {
      case DW_AT_call_file: {
        if (data == 0 || data >= file_names_->size()) {
          LOG(WARNING) << "unexpected reference to file_num " << data;
          break;
        }

        if (file_names_ != NULL) {
          const FileVector::value_type &file =
              (*file_names_)[data];
          if (directory_names_ != NULL) {
            if (file.first < directory_names_->size()) {
              const char *dir = (*directory_names_)[file.first];
              subprogram_stack_.back()->set_callsite_directory(dir);
            } else {
              LOG(WARNING) << "unexpected reference to dir_num " << file.first;
            }
          }
          subprogram_stack_.back()->set_callsite_filename(file.second);
        }
        break;
      }
      case DW_AT_call_line:
        CHECK(form == DW_FORM_data1 ||
              form == DW_FORM_data2 ||
              form == DW_FORM_data4);
        subprogram_stack_.back()->set_callsite_line(data);
        break;
      case DW_AT_GNU_discriminator:
        CHECK(form == DW_FORM_data1 ||
              form == DW_FORM_data2 ||
              form == DW_FORM_data4);
        subprogram_stack_.back()->set_callsite_discr(data);
        break;
      case DW_AT_abstract_origin:
        CHECK(form == DW_FORM_ref4);
        subprogram_stack_.back()->set_abstract_origin(
            compilation_unit_offset_ + data);
        break;
      case DW_AT_specification:
        CHECK(form == DW_FORM_ref4);
        subprogram_stack_.back()->set_specification(
            compilation_unit_offset_ + data);
        break;
      case DW_AT_low_pc:
        subprogram_stack_.back()->SetSingletonRangeLow(data);
        break;
      case DW_AT_high_pc:
        subprogram_stack_.back()->SetSingletonRangeHigh(
            data, form != DW_FORM_addr);
        break;
      case DW_AT_ranges: {
        CHECK_EQ(0, subprogram_stack_.back()->address_ranges()->size());
        AddressRangeList::RangeList ranges;
        address_ranges_->ReadRangeList(data, compilation_unit_base_, &ranges);
        subprogram_stack_.back()->SwapAddressRanges(&ranges);
        break;
      }
      case DW_AT_decl_line: {
        if (die_stack_.back() == DW_TAG_subprogram) {
          subprogram_stack_.back()->set_callsite_line(data);
        }
        break;
      }
      default:
        break;
    }
  } else if (die_stack_.back() == DW_TAG_compile_unit) {
    // The subprogram stack is empty.  This information is therefore
    // describing the compilation unit.
    switch (attr) {
      case DW_AT_low_pc:
        compilation_unit_base_ = data;
        break;
      case DW_AT_stmt_list:
        {
          SectionMap::const_iterator iter = sections_.find(".debug_line");
          CHECK(iter != sections_.end()) << "unable to find .debug_line "
              "in section map";
          LineInfo lireader(iter->second.first + data,
                                          iter->second.second - data,
                                          reader_, line_handler_);
          lireader.Start();
        }
        break;
      default:
        break;
    }
  }
}

void InlineStackHandler::FindBadSubprograms(
    set<const SubprogramInfo *> *bad_subprograms) {
  // Search for bad DIEs.  The debug information often contains
  // multiple entries for the same function.  However, only one copy
  // of the debug information corresponds to the actual emitted code.
  // The others may be correct (if they got compiled identically) or
  // they may be wrong.  This code filters out bad debug information
  // using two approaches:
  //
  // 1) If a non-inlined function's address ranges contain the
  //    starting address of other non-inlined functions, then it is
  //    bad.  This approach is safe because the starting address for
  //    functions is accurate across all the DIEs.
  //
  // 2) If multiple functions start at the same address after pruning
  //    using phase one, then pick the largest one.  This heuristic is
  //    based on the assumption that if the largest one were bad,
  //    then it would have conflicted with another function and would have
  //    been pruned in step 1.

  // Find the start addresses for each non-inlined subprogram.
  set<uint64> start_addresses;
  for (vector<SubprogramInfo *>::iterator subprogs =
           subprogram_insert_order_.begin();
       subprogs != subprogram_insert_order_.end();
       ++subprogs) {
    SubprogramInfo *subprog = *subprogs;
    if (subprog->inlined())
      continue;

    uint64 start_address =
        AddressRangeList::RangesMin(subprog->address_ranges());
    start_addresses.insert(start_address);
  }

  // Find bad non-inlined subprograms according to rule (1) above.
  for (vector<SubprogramInfo *>::iterator subprogs =
           subprogram_insert_order_.begin();
       subprogs != subprogram_insert_order_.end();
       ++subprogs) {
    SubprogramInfo *subprog = *subprogs;
    if (subprog->inlined())
      continue;

    typedef AddressRangeList::RangeList RangeList;
    const RangeList *ranges = subprog->address_ranges();
    uint64 min_address = AddressRangeList::RangesMin(ranges);
    uint64 max_address = AddressRangeList::RangesMax(ranges);

    set<uint64>::iterator closest_match =
        start_addresses.lower_bound(min_address);

    if (closest_match != start_addresses.end() &&
        (*closest_match) == min_address)
      ++closest_match;

    if (closest_match != start_addresses.end() &&
        (*closest_match) < max_address)
      bad_subprograms->insert(subprog);
  }

  // Find the bad non-inlined subprograms according to rule (2) above.
  map<uint64, SubprogramInfo *> good_subprograms;
  for (vector<SubprogramInfo *>::iterator subprogs =
           subprogram_insert_order_.begin();
       subprogs != subprogram_insert_order_.end();
       ++subprogs) {
    SubprogramInfo *subprog = *subprogs;

    // Filter out non-inlined subprograms
    if (subprog->inlined())
      continue;

    // Filter out subprograms with no range information
    if (subprog->address_ranges()->size() == 0)
      continue;

    // Filter out bad subprograms
    if (bad_subprograms->find(subprog) != bad_subprograms->end())
      continue;

    // See if there is another subprogram at this address
    uint64 start_address = AddressRangeList::RangesMin(
        subprog->address_ranges());
    map<uint64, SubprogramInfo *>::iterator other =
        good_subprograms.find(start_address);

    if (other == good_subprograms.end()) {
      // If there isn't, then update the map
      good_subprograms[start_address] = subprog;
    } else {
      // If there is, update the map if this function is bigger
      uint64 end_address = AddressRangeList::RangesMax(
          subprog->address_ranges());
      uint64 other_end_address = AddressRangeList::RangesMax(
          other->second->address_ranges());

      if (end_address > other_end_address) {
        good_subprograms[start_address] = subprog;
        bad_subprograms->insert(other->second);
      } else {
        bad_subprograms->insert(subprog);
      }
    }
  }

  // Expand the set of bad subprograms to include inlined subprograms.
  // An inlined subprogram is bad if its parent is bad.  Since
  // subprograms are stored in a leaf-to-parent order in
  // subprogram_insert_order_, it suffices to scan the vector
  // backwards once.
  for (vector<SubprogramInfo *>::reverse_iterator subprogs =
           subprogram_insert_order_.rbegin();
       subprogs != subprogram_insert_order_.rend();
       ++subprogs) {
    SubprogramInfo *subprog = *subprogs;
    if (bad_subprograms->find(subprog->parent()) != bad_subprograms->end()) {
      bad_subprograms->insert(subprog);
    }
  }
}

void InlineStackHandler::PopulateSubprogramsByAddress() {
  // This variable should no longer be accessed.  Let's set it to NULL
  // here since this is the first opportunity to do so.
  address_ranges_ = NULL;

  set<const SubprogramInfo *> bad_subprograms;
  FindBadSubprograms(&bad_subprograms);

  // For the DIEs that are not marked bad, insert them into the
  // address based map.
  for (vector<SubprogramInfo *>::iterator subprogs =
           subprogram_insert_order_.begin();
       subprogs != subprogram_insert_order_.end();
       ++subprogs) {
    SubprogramInfo *subprog = *subprogs;

    if (bad_subprograms.find(subprog) == bad_subprograms.end())
      subprograms_by_address_.InsertRangeList(
          *subprog->address_ranges(), subprog);
  }

  // Clear this vector to save some memory
  subprogram_insert_order_.clear();
}

const SubprogramInfo *InlineStackHandler::GetSubprogramForAddress(
    uint64 address) {
  NonOverlappingRangeMap<SubprogramInfo*>::ConstIterator iter =
      subprograms_by_address_.Find(address);
  if (iter != subprograms_by_address_.End())
    return iter->second;
  else
    return NULL;
}

const SubprogramInfo *InlineStackHandler::GetDeclaration(
    const SubprogramInfo *subprog) const {
  const int cu_index = subprog->cu_index();
  const SubprogramInfo *declaration = subprog;
  CHECK(cu_index < subprograms_by_offset_maps_.size());
  SubprogramsByOffsetMap* subprograms_by_offset =
      subprograms_by_offset_maps_[cu_index];
  while (declaration->name().empty() || declaration->callsite_line() == 0) {
    uint64 specification = declaration->specification();
    if (specification) {
      declaration = subprograms_by_offset->find(specification)->second;
    } else {
      uint64 abstract_origin = declaration->abstract_origin();
      if (abstract_origin)
        declaration = subprograms_by_offset->find(abstract_origin)->second;
      else
        break;
    }
  }
  return declaration;
}

const SubprogramInfo *InlineStackHandler::GetAbstractOrigin(
    const SubprogramInfo *subprog) const {
  const int cu_index = subprog->cu_index();
  CHECK(cu_index < subprograms_by_offset_maps_.size());
  SubprogramsByOffsetMap* subprograms_by_offset =
      subprograms_by_offset_maps_[cu_index];
  if (subprog->abstract_origin())
    return subprograms_by_offset->find(subprog->abstract_origin())->second;
  else
    return subprog;
}

void InlineStackHandler::GetSubprogramAddresses(set<uint64> *addrs) {
  for (auto it = subprograms_by_address_.Begin();
       it != subprograms_by_address_.End(); ++it) {
    addrs->insert(it->first.first);
  }
}

InlineStackHandler::~InlineStackHandler() {
  for (auto map : subprograms_by_offset_maps_) {
    for (const auto &addr_subprog : *map)
      delete addr_subprog.second;
    delete map;
  }
  for (auto comp_dir : compilation_unit_comp_dir_)
    delete comp_dir;
}

}  // namespace autofdo
