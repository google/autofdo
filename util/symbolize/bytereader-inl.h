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

#ifndef AUTOFDO_SYMBOLIZE_BYTEREADER_INL_H__
#define AUTOFDO_SYMBOLIZE_BYTEREADER_INL_H__

#include <stddef.h>

#include "base/common.h"
#include "symbolize/bytereader.h"

namespace autofdo {

inline uint8 ByteReader::ReadOneByte(const char* buffer) const {
  return buffer[0];
}

inline uint16 ByteReader::ReadTwoBytes(const char* buffer) const {
  const uint16 buffer0 = static_cast<uint16>(buffer[0]) & 0xff;
  const uint16 buffer1 = static_cast<uint16>(buffer[1]) & 0xff;
  if (endian_ == ENDIANNESS_LITTLE) {
    return buffer0 | buffer1 << 8;
  } else {
    return buffer1 | buffer0 << 8;
  }
}

inline uint64 ByteReader::ReadFourBytes(const char* buffer) const {
  const uint32 buffer0 = static_cast<uint32>(buffer[0]) & 0xff;
  const uint32 buffer1 = static_cast<uint32>(buffer[1]) & 0xff;
  const uint32 buffer2 = static_cast<uint32>(buffer[2]) & 0xff;
  const uint32 buffer3 = static_cast<uint32>(buffer[3]) & 0xff;
  if (endian_ == ENDIANNESS_LITTLE) {
    return buffer0 | buffer1 << 8 | buffer2 << 16 | buffer3 << 24;
  } else {
    return buffer3 | buffer2 << 8 | buffer1 << 16 | buffer0 << 24;
  }
}

inline uint64 ByteReader::ReadEightBytes(const char* buffer) const {
  const uint64 buffer0 = static_cast<uint64>(buffer[0]) & 0xff;
  const uint64 buffer1 = static_cast<uint64>(buffer[1]) & 0xff;
  const uint64 buffer2 = static_cast<uint64>(buffer[2]) & 0xff;
  const uint64 buffer3 = static_cast<uint64>(buffer[3]) & 0xff;
  const uint64 buffer4 = static_cast<uint64>(buffer[4]) & 0xff;
  const uint64 buffer5 = static_cast<uint64>(buffer[5]) & 0xff;
  const uint64 buffer6 = static_cast<uint64>(buffer[6]) & 0xff;
  const uint64 buffer7 = static_cast<uint64>(buffer[7]) & 0xff;
  if (endian_ == ENDIANNESS_LITTLE) {
    return buffer0 | buffer1 << 8 | buffer2 << 16 | buffer3 << 24 |
      buffer4 << 32 | buffer5 << 40 | buffer6 << 48 | buffer7 << 56;
  } else {
    return buffer7 | buffer6 << 8 | buffer5 << 16 | buffer4 << 24 |
      buffer3 << 32 | buffer2 << 40 | buffer1 << 48 | buffer0 << 56;
  }
}

// Read an unsigned LEB128 number.  Each byte contains 7 bits of
// information, plus one bit saying whether the number continues or
// not.

inline uint64 ByteReader::ReadUnsignedLEB128(const char* buffer,
                                             size_t* len) const {
  uint64 result = 0;
  size_t num_read = 0;
  unsigned int shift = 0;
  unsigned char byte;

  do {
    byte = *buffer++;
    num_read++;

    result |= (static_cast<uint64>(byte & 0x7f)) << shift;

    shift += 7;
  } while (byte & 0x80);

  *len = num_read;

  return result;
}

// Read a signed LEB128 number.  These are like regular LEB128
// numbers, except the last byte may have a sign bit set.

inline int64 ByteReader::ReadSignedLEB128(const char* buffer,
                                          size_t* len) const {
  int64 result = 0;
  int shift = 0;
  size_t num_read = 0;
  unsigned char byte;

  do {
      byte = *buffer++;
      num_read++;
      result |= (static_cast<uint64>(byte & 0x7f) << shift);
      shift += 7;
  } while (byte & 0x80);

  if ((shift < 8 * sizeof (result)) && (byte & 0x40))
    result |= -((static_cast<int64>(1)) << shift);
  *len = num_read;
  return result;
}

inline uint64 ByteReader::ReadOffset(const char* buffer) const {
  CHECK(this->offset_reader_);
  return (this->*offset_reader_)(buffer);
}

inline uint64 ByteReader::ReadAddress(const char* buffer) const {
  CHECK(this->address_reader_);
  return (this->*address_reader_)(buffer);
}

}  // namespace autofdo

#endif  // AUTOFDO_SYMBOLIZE_BYTEREADER_INL_H__
