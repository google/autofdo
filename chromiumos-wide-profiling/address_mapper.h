// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_ADDRESS_MAPPER_H_
#define CHROMIUMOS_WIDE_PROFILING_ADDRESS_MAPPER_H_

#include <stdint.h>

#include <list>

namespace quipper {

class AddressMapper {
 public:
  AddressMapper() {}

  // Copy constructor: copies mappings from |source| to this AddressMapper. This
  // is useful for copying mappings from parent to child process upon fork(). It
  // is also useful to copy kernel mappings to any process that is created.
  AddressMapper(const AddressMapper& source);

  // Maps a new address range to quipper space.
  // |remove_existing_mappings| indicates whether to remove old mappings that
  // collide with the new range in real address space, indicating it has been
  // unmapped.
  // Returns true if mapping was successful.
  bool Map(const uint64_t real_addr,
           const uint64_t length,
           bool remove_existing_mappings);

  // Like Map(real_addr, length, remove_existing_mappings).  |id| is an
  // identifier value to be stored along with the mapping.  AddressMapper does
  // not care whether it is unique compared to all other IDs passed in.  That is
  // up to the caller to keep track of.
  // |offset_base| represents the offset within the original region at which the
  // mapping begins. The original region can be much larger than the mapped
  // region.
  // e.g. Given a mapped region with base=0x4000 and size=0x2000 mapped with
  // offset_base=0x10000, then the address 0x5000 maps to an offset of 0x11000
  // (0x5000 - 0x4000 + 0x10000).
  bool MapWithID(const uint64_t real_addr,
                 const uint64_t length,
                 const uint64_t id,
                 const uint64_t offset_base,
                 bool remove_existing_mappings);

  // Looks up |real_addr| and returns the mapped address.
  bool GetMappedAddress(const uint64_t real_addr, uint64_t* mapped_addr) const;

  // Looks up |real_addr| and returns the mapping's ID and offset from the
  // start of the mapped space.
  bool GetMappedIDAndOffset(const uint64_t real_addr,
                            uint64_t* id,
                            uint64_t* offset) const;

  // Returns true if there are no mappings.
  bool IsEmpty() const {
    return mappings_.empty();
  }

  // Returns the number of address ranges that are currently mapped.
  unsigned int GetNumMappedRanges() const {
    return mappings_.size();
  }

  // Returns the maximum length of quipper space containing mapped areas.
  // There may be gaps in between blocks.
  // If the result is 2^64 (all of quipper space), this returns 0.  Call
  // IsEmpty() to distinguish this from actual emptiness.
  uint64_t GetMaxMappedLength() const;

  // Dumps the state of the address mapper to logs. Useful for debugging.
  void DumpToLog() const;

 private:
  struct MappedRange {
    uint64_t real_addr;
    uint64_t mapped_addr;
    uint64_t size;

    uint64_t id;
    uint64_t offset_base;

    // Length of unmapped space after this range.
    uint64_t unmapped_space_after;

    // Determines if this range intersects another range in real space.
    inline bool Intersects(const MappedRange& range) const {
      return (real_addr <= range.real_addr + range.size - 1) &&
             (real_addr + size - 1 >= range.real_addr);
    }

    // Determines if this range fully covers another range in real space.
    inline bool Covers(const MappedRange& range) const {
      return (real_addr <= range.real_addr) &&
             (real_addr + size - 1 >= range.real_addr + range.size - 1);
    }

    // Determines if this range fully contains another range in real space.
    // This is different from Covers() in that the boundaries cannot overlap.
    inline bool Contains(const MappedRange& range) const {
      return (real_addr < range.real_addr) &&
             (real_addr + size - 1 > range.real_addr + range.size - 1);
    }

    // Determines if this range contains the given address |addr|.
    inline bool ContainsAddress(uint64_t addr) const {
      return (addr >= real_addr && addr <= real_addr + size - 1);
    }
  };

  // TODO(sque): implement with set or map to improve searching.
  typedef std::list<MappedRange> MappingList;

  // Removes an existing address mapping.
  // Returns true if successful, false if no mapped address range was found.
  bool Unmap(const MappedRange& range);

  // Container for all the existing mappings.
  MappingList mappings_;

  bool CheckMappings() const;
};

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_ADDRESS_MAPPER_H_
