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

#ifndef AUTOFDO_SYMBOLIZE_BYTEREADER_H__
#define AUTOFDO_SYMBOLIZE_BYTEREADER_H__

#include <stddef.h>

#include "base/common.h"

namespace autofdo {

// We can't use the obvious name of LITTLE_ENDIAN and BIG_ENDIAN
// because it conflicts with a macro
enum Endianness {
  ENDIANNESS_BIG,
  ENDIANNESS_LITTLE,
  ENDIANNESS_NATIVE = (__BYTE_ORDER == __BIG_ENDIAN ? ENDIANNESS_BIG
                                                    : ENDIANNESS_LITTLE)
};

// Class that knows how to read both big endian and little endian
// numbers, for use in DWARF2/3 reader.
// Takes an endianness argument.
// To read addresses and offsets, SetAddressSize and SetOffsetSize
// must be called first.
class ByteReader {
 public:
  explicit ByteReader(enum Endianness endian);
  virtual ~ByteReader();

  // Set the address size to SIZE, which sets up the ReadAddress member
  // so that it works.
  void SetAddressSize(uint8 size);

  // Set the offset size to SIZE, which sets up the ReadOffset member
  // so that it works.
  void SetOffsetSize(uint8 size);

  // Return the current offset size
  uint8 OffsetSize() const { return offset_size_; }

  // Return the current address size
  uint8 AddressSize() const { return address_size_; }

  // Read a single byte from BUFFER and return it as an unsigned 8 bit
  // number.
  uint8 ReadOneByte(const char* buffer) const;

  // Read two bytes from BUFFER and return it as an unsigned 16 bit
  // number.
  uint16 ReadTwoBytes(const char* buffer) const;

  // Read four bytes from BUFFER and return it as an unsigned 32 bit
  // number.  This function returns a uint64 so that it is compatible
  // with ReadAddress and ReadOffset.  The number it returns will
  // never be outside the range of an unsigned 32 bit integer.
  uint64 ReadFourBytes(const char* buffer) const;

  // Read eight bytes from BUFFER and return it as an unsigned 64 bit
  // number
  uint64 ReadEightBytes(const char* buffer) const;

  // Read an unsigned LEB128 (Little Endian Base 128) number from
  // BUFFER and return it as an unsigned 64 bit integer.  LEN is set
  // to the length read.  Everybody seems to reinvent LEB128 as a
  // variable size integer encoding, DWARF has had it for a long time.
  uint64 ReadUnsignedLEB128(const char* buffer, size_t* len) const;

  // Read a signed LEB128 number from BUFFER and return it as an
  // signed 64 bit integer.  LEN is set to the length read.
  int64 ReadSignedLEB128(const char* buffer, size_t* len) const;

  // Read an offset from BUFFER and return it as an unsigned 64 bit
  // integer.  DWARF2/3 define offsets as either 4 or 8 bytes,
  // generally depending on the amount of DWARF2/3 info present.
  uint64 ReadOffset(const char* buffer) const;

  // Read an address from BUFFER and return it as an unsigned 64 bit
  // integer.  DWARF2/3 allow addresses to be any size from 0-255
  // bytes currently.  Internally we support 4 and 8 byte addresses,
  // and will CHECK on anything else.
  uint64 ReadAddress(const char* buffer) const;

 private:
  // Function pointer type for our address and offset readers.
  typedef uint64 (ByteReader::*AddressReader)(const char*) const;

  // Read an offset from BUFFER and return it as an unsigned 64 bit
  // integer.  DWARF2/3 define offsets as either 4 or 8 bytes,
  // generally depending on the amount of DWARF2/3 info present.
  // This function pointer gets set by SetOffsetSize.
  AddressReader offset_reader_;

  // Read an address from BUFFER and return it as an unsigned 64 bit
  // integer.  DWARF2/3 allow addresses to be any size from 0-255
  // bytes currently.  Internally we support 4 and 8 byte addresses,
  // and will CHECK on anything else.
  // This function pointer gets set by SetAddressSize.
  AddressReader address_reader_;

  Endianness endian_;
  uint8 address_size_;
  uint8 offset_size_;
  DISALLOW_EVIL_CONSTRUCTORS(ByteReader);
};

}  // namespace autofdo

#endif  // AUTOFDO_SYMBOLIZE_BYTEREADER_H__
