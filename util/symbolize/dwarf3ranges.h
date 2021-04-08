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

#include "base/common.h"
#include "symbolize/bytereader.h"

namespace autofdo {

// This class represents a DWARF3 non-contiguous address range.  The
// contents of an address range section are passed in
// (e.g. .debug_ranges) and subsequently, an interpretation of any
// offset in the section can be requested.
class AddressRangeList {
 public:
  typedef pair<uint64, uint64> Range;
  typedef vector<Range> RangeList;
  AddressRangeList(const char* buffer,
                   uint64 buffer_length,
                   ByteReader* reader)
      : reader_(reader),
        buffer_(buffer),
        buffer_length_(buffer_length) { }

  void ReadRangeList(uint64 offset, uint64 base,
                     RangeList* output);

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

 private:
  // The associated ByteReader that handles endianness issues for us
  ByteReader* reader_;

  // buffer is the buffer for our range info
  const char* buffer_;
  uint64 buffer_length_;
  DISALLOW_COPY_AND_ASSIGN(AddressRangeList);
};

}  // namespace autofdo

#endif  // AUTOFDO_SYMBOLIZE_DWARF3RANGES_H_
