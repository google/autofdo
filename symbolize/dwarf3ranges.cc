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

#include "symbolize/dwarf3ranges.h"

#include "base/logging.h"
#include "symbolize/bytereader.h"
#include "symbolize/bytereader-inl.h"

namespace autofdo {

void AddressRangeList::ReadRangeList(uint64 offset, uint64 base,
                                     AddressRangeList::RangeList* ranges) {
  uint8 width = reader_->AddressSize();

  uint64 largest_address;
  if (width == 4)
    largest_address = 0xffffffffL;
  else if (width == 8)
    largest_address = 0xffffffffffffffffLL;
  else
    LOG(FATAL) << "width==" << width << " must be 4 or 8";

  const char* pos = buffer_ + offset;
  do {
    CHECK((pos + 2*width) <= (buffer_ + buffer_length_));
    uint64 start = reader_->ReadAddress(pos);
    uint64 stop = reader_->ReadAddress(pos+width);
    if (start == largest_address)
      base = stop;
    else if (start == 0 && stop == 0)
      break;
    else
      ranges->push_back(make_pair(start+base, stop+base));
    pos += 2*width;
  } while (true);
}

}  // namespace autofdo
