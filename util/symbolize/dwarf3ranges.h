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

#ifndef AUTOFDO_SYMBOLIZE_DWARF3RANGES_H_
#define AUTOFDO_SYMBOLIZE_DWARF3RANGES_H_

#include <algorithm>
#include <utility>
#include <vector>
#include <memory>
#include <map>

#include "base/common.h"
#include "symbolize/bytereader.h"

namespace devtools_crosstool_autofdo {

// Entry kinds for DWARF5 non-contiguous address ranges
enum DwarfRangeListEntryKind {
  DW_RLE_end_of_list = 0,
  DW_RLE_base_addressx = 1,
  DW_RLE_startx_endx = 2,
  DW_RLE_startx_length = 3,
  DW_RLE_offset_pair = 4,
  DW_RLE_base_address = 5,
  DW_RLE_start_end = 6,
  DW_RLE_start_length = 7
};


// This struct contains header info and each range list specific data.
struct RngListsData {
  RngListsData()
  : unit_length(0),
    version(0),
    address_size(0),
    segment_selector_size(0),
    offset_entry_count(0),
    rnglist_base_(0),
    offset_list_() {}

  uint64 unit_length;
  uint8 version; 
  uint8 address_size;
  uint8 segment_selector_size;
  uint32 offset_entry_count;
  uint64 rnglist_base_;
  std::vector<uint32> offset_list_;
};

// This class represents a DWARF3 non-contiguous address range.  The
// contents of an address range section are passed in
// (e.g. .debug_ranges) and subsequently, an interpretation of any
// offset in the section can be requested.
class AddressRangeList {
 public:
  typedef pair<uint64, uint64> Range;
  typedef vector<Range> RangeList;
  typedef std::map<uint64, RngListsData> RngDataMap;
  AddressRangeList(const char* ranges_buffer,
                   uint64 ranges_buffer_length,
                   const char* rnglist_buffer,
                   uint64 rnglist_buffer_length,
                   ByteReader* reader,
                   const char* addr_buffer,
                   uint64 addr_buffer_length)
      : reader_(reader),
        ranges_buffer_(ranges_buffer),
        ranges_buffer_length_(ranges_buffer_length),
        rnglist_buffer_(rnglist_buffer),
        rnglist_buffer_length_(rnglist_buffer_length),
        addr_buffer_(addr_buffer),
        addr_buffer_length_(addr_buffer_length),
        rngdatamap_() {
      if (rnglist_buffer != NULL) {
          ReadDwarfRngListsHeader();
      }
  }

  void ReadRangeList(uint64 offset, uint64 base,
                     RangeList* output, uint8 dwarf_version, uint64 addr_base = 0);

  // This handles the case where we read ranges with DW_FORM_sec_offset.
  // In this case, the rnglist_buffer_ does not have offset array. So for calculating the position,
  // we only add the offset to rnglist_buffer_.
  void ReadDwarfRngListsDirectly(uint64 offset, uint64 base,
                                 AddressRangeList::RangeList* ranges, uint64 addr_base);

  // In this case DW_FORM_rnglistx, rnglist_buffer_ includes offset array too.
  // So we have to add that to the the rnglist_buffer_ to be able to read the RngLists.
  void ReadDwarfRngListwithOffsetArray(uint64 offset, uint64 base,
                                       AddressRangeList::RangeList* ranges, 
                                       uint64 addr_base, uint64 range_base_);

  static uint64 RangesMin(const RangeList *ranges) {
    if (ranges->size() == 0)
      return 0;

    uint64 result = kint64max;
    for (AddressRangeList::RangeList::const_iterator iter =
             ranges->begin();
         iter != ranges->end(); ++iter) {
      result = min(result, iter->first);
    }
    return result;
  }

  uint64 GetRngListsElementOffsetByIndex(uint64 addr_base, uint64 rng_index);

 private:

  uint64 ReadOffset(const char** offsetarrayptr);

  void ReadDwarfRngListsHeader();

  void ReadDwarfRangeList(uint64 offset, uint64 base,
                           RangeList* output);

  void ReadDwarfRngLists(uint64 base, RangeList* output, const char* pos, uint64 addr_base, uint8 address_size);



  // The associated ByteReader that handles endianness issues for us
  ByteReader* reader_;

  const char* ranges_buffer_;
  uint64 ranges_buffer_length_;
  const char* rnglist_buffer_;
  uint64 rnglist_buffer_length_;
  const char* addr_buffer_;
  uint64 addr_buffer_length_;

  // a map of addr_base_ -> RngListsData
  // If the RngList we are processing does not have offset array,
  // then addr_base will be the beginning of the data address (right after header).
  RngDataMap rngdatamap_;

  DISALLOW_COPY_AND_ASSIGN(AddressRangeList);
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_SYMBOLIZE_DWARF3RANGES_H_
