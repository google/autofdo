// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/address_mapper.h"

#include "base/logging.h"

#include "chromiumos-wide-profiling/limits.h"
#include <vector>

namespace quipper {

AddressMapper::AddressMapper(const AddressMapper& source) {
  mappings_ = source.mappings_;
  real_address_map_ = source.real_address_map_;
}

bool AddressMapper::Map(const uint64_t real_addr,
                        const uint64_t size,
                        const bool remove_existing_mappings) {
  return MapWithID(real_addr, size, kUint64Max, 0, remove_existing_mappings);
}

bool AddressMapper::MapWithID(const uint64_t real_addr,
                              const uint64_t size,
                              const uint64_t id,
                              const uint64_t offset_base,
                              bool remove_existing_mappings) {
  MappedRange range;
  range.real_addr = real_addr;
  range.size = size;
  range.id = id;
  range.offset_base = offset_base;

  if (size == 0) {
    LOG(ERROR) << "Must allocate a nonzero-length address range.";
    return false;
  }

  // Check that this mapping does not overflow the address space.
  if (real_addr + size - 1 != kUint64Max &&
      !(real_addr + size > real_addr)) {
    DumpToLog();
    LOG(ERROR) << "Address mapping at " << std::hex << real_addr
               << " with size " << std::hex << size << " overflows.";
    return false;
  }

  // Check for collision with an existing mapping.  This must be an overlap that
  // does not result in one range being completely covered by another
  MappingList::iterator iter;
  if (!mappings_.empty()) {
    std::vector<MappedRange> mappings_to_delete;
    bool old_range_found = false;
    MappedRange old_range;

    iter = find(real_addr);
    if (iter != mappings_.begin() && iter->second.real_addr > real_addr)
      --iter;
    for (;
         iter != mappings_.end() && iter->second.real_addr < (real_addr + size);
         ++iter) {
      if (!iter->second.Intersects(range))
        continue;
      // Quit if existing ranges that collide aren't supposed to be removed.
      if (!remove_existing_mappings)
        return false;
      if (!old_range_found && iter->second.Covers(range) &&
          iter->second.size > range.size) {
        old_range_found = true;
        old_range = iter->second;
        continue;
      }
      mappings_to_delete.push_back(iter->second);
    }
    for (const MappedRange &R : mappings_to_delete)
      CHECK(Unmap(R));

    // Otherwise check for this range being covered by another range.  If that
    // happens, split or reduce the existing range to make room.
    if (old_range_found) {
      CHECK(Unmap(old_range));

      uint64_t gap_before = range.real_addr - old_range.real_addr;
      uint64_t gap_after = (old_range.real_addr + old_range.size) -
                           (range.real_addr + range.size);

      if (gap_before) {
        CHECK(MapWithID(old_range.real_addr,
                        gap_before,
                        old_range.id,
                        old_range.offset_base,
                        false));
      }

      CHECK(MapWithID(range.real_addr, range.size, id, offset_base, false));

      if (gap_after) {
        CHECK(MapWithID(range.real_addr + range.size,
                        gap_after,
                        old_range.id,
                        old_range.offset_base + gap_before + range.size,
                        false));
      }
      return true;
    }
  }

  // Now search for a location for the new range.  It should be in the first
  // free block in quipper space.

  // If there is no existing mapping, add it to the beginning of quipper space.
  if (mappings_.empty()) {
    range.mapped_addr = 0;
    range.unmapped_space_after = kUint64Max - range.size;
    insert(range);
    return true;
  }
  CHECK(!real_address_map_.empty());

  // If there is space before the first mapped range in quipper space, use it.
  uint64_t first_mapped = real_address_map_.begin()->first;
  if (first_mapped >= range.size) {
    range.mapped_addr = 0;
    range.unmapped_space_after = first_mapped - range.size;
    insert(range);
    return true;
  }

  // Otherwise, search through the existing mappings for a free block after one
  // of them.
  MappingList::iterator first = mappings_.end();
  uint64_t last_mapped_start = real_address_map_.rbegin()->first;
  for (iter = mappings_.begin(); iter != mappings_.end(); ++iter) {
    if (iter->second.unmapped_space_after < size)
      continue;
    first = iter;
    if (iter->second.mapped_addr != last_mapped_start)
      break;
  }
  if (first != mappings_.end()) {
    range.mapped_addr = first->second.mapped_addr + first->second.size;
    range.unmapped_space_after =
        first->second.unmapped_space_after - range.size;
    first->second.unmapped_space_after = 0;
    insert(range);
    return true;
  }

  // If it still hasn't succeeded in mapping, it means there is no free space in
  // quipper space large enough for a mapping of this size.
  DumpToLog();
  LOG(ERROR) << "Could not find space to map addr=" << std::hex << real_addr
             << " with size " << std::hex << size;
  return false;
}

void AddressMapper::DumpToLog() const {
  MappingList::const_iterator it;
  for (it = mappings_.begin(); it != mappings_.end(); ++it) {
    LOG(INFO) << " real_addr: " << std::hex << it->second.real_addr
              << " mapped: " << std::hex << it->second.mapped_addr
              << " id: " << std::hex << it->second.id
              << " size: " << std::hex << it->second.size;
  }
}

bool AddressMapper::GetMappedAddress(const uint64_t real_addr,
                                     uint64_t* mapped_addr) const {
  CHECK(mapped_addr);
  MappingList::const_iterator iter = find(real_addr);
  if (!iter->second.ContainsAddress(real_addr))
    return false;
  *mapped_addr = iter->second.mapped_addr + real_addr - iter->second.real_addr;
  return true;
}

bool AddressMapper::GetMappedIDAndOffset(const uint64_t real_addr,
                                         uint64_t* id,
                                         uint64_t* offset) const {
  CHECK(id);
  CHECK(offset);
  MappingList::const_iterator iter = find(real_addr);
  if (!iter->second.ContainsAddress(real_addr))
    return false;

  *id = iter->second.id;
  *offset = real_addr - iter->second.real_addr + iter->second.offset_base;
  return true;
}

uint64_t AddressMapper::GetMaxMappedLength() const {
  if (IsEmpty())
    return 0;

  // The first mapped address
  uint64_t min = real_address_map_.begin()->first;
  // The real address start of the last mapped range
  uint64_t last_real_start = real_address_map_.rbegin()->second;
  auto iter = find(last_real_start);
  uint64_t last_addr = iter->second.mapped_addr;
  // The end of the last mapped range
  uint64_t max = last_addr + iter->second.size;
  return max - min;
}

bool AddressMapper::Unmap(const MappedRange &range) {

  // Find the range to be removed
  auto range_iter = mappings_.find(range.real_addr);
  if (!range_iter->second.Matches(range))
    return false;

  // Find the range that preceeds this in mapped space.
  auto iter = real_address_map_.find(range_iter->second.mapped_addr);
  if (iter != real_address_map_.begin()) {
    auto prev = iter;
    --prev;
    // update the corresponding range entry
    auto prev_range_iter = mappings_.find(prev->second);
        prev_range_iter->second.unmapped_space_after +=
          range.size + range.unmapped_space_after;
  }
  real_address_map_.erase(iter);
  mappings_.erase(range_iter);
  return true;
}

void AddressMapper::insert(const MappedRange &range) {
  mappings_.emplace(range.real_addr, range);
  real_address_map_.emplace(range.mapped_addr, range.real_addr);
}

AddressMapper::MappingList::iterator AddressMapper::find(uint64_t addr) {
  auto iter = mappings_.upper_bound(addr);
  if (iter != mappings_.begin())
    --iter;
  return iter;
}
AddressMapper::MappingList::const_iterator
AddressMapper::find(uint64_t addr) const {
  auto iter = mappings_.upper_bound(addr);
  if (iter != mappings_.begin())
    --iter;
  return iter;
}

}  // namespace quipper
