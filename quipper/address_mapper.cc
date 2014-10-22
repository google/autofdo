// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "address_mapper.h"

#include "base/logging.h"
#include "base/basictypes.h"

namespace quipper {

AddressMapper::AddressMapper(const AddressMapper& source) {
  mappings_ = source.mappings_;
}

bool AddressMapper::Map(const uint64_t real_addr,
                        const uint64_t size,
                        const bool remove_existing_mappings) {
  return MapWithID(real_addr, size, kuint64max, 0, remove_existing_mappings);
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
  if (real_addr + size - 1 != kuint64max &&
      !(real_addr + size > real_addr)) {
    DumpToLog();
    LOG(ERROR) << "Address mapping at " << std::hex << real_addr
               << " with size " << std::hex << size << " overflows.";
    return false;
  }

  // Check for collision with an existing mapping.  This must be an overlap that
  // does not result in one range being completely covered by another
  MappingList::iterator iter;
  MappingList mappings_to_delete;
  bool old_range_found = false;
  MappedRange old_range;
  for (iter = mappings_.begin(); iter != mappings_.end(); ++iter) {
    if (!iter->Intersects(range))
      continue;
    // Quit if existing ranges that collide aren't supposed to be removed.
    if (!remove_existing_mappings)
      return false;
    if (!old_range_found && iter->Covers(range) && iter->size > range.size) {
      old_range_found = true;
      old_range = *iter;
      continue;
    }
    mappings_to_delete.push_back(*iter);
  }

  while (!mappings_to_delete.empty()) {
    const MappedRange& range = mappings_to_delete.front();
    CHECK(Unmap(range));
    mappings_to_delete.pop_front();
  }

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

  // Now search for a location for the new range.  It should be in the first
  // free block in quipper space.

  // If there is no existing mapping, add it to the beginning of quipper space.
  if (mappings_.empty()) {
    range.mapped_addr = 0;
    range.unmapped_space_after = kuint64max - range.size;
    mappings_.push_back(range);
    return true;
  }

  // If there is space before the first mapped range in quipper space, use it.
  if (mappings_.begin()->mapped_addr >= range.size) {
    range.mapped_addr = 0;
    range.unmapped_space_after = mappings_.begin()->mapped_addr - range.size;
    mappings_.push_front(range);
    return true;
  }

  // Otherwise, search through the existing mappings for a free block after one
  // of them.
  for (iter = mappings_.begin(); iter != mappings_.end(); ++iter) {
    if (iter->unmapped_space_after < range.size)
      continue;

    range.mapped_addr = iter->mapped_addr + iter->size;
    range.unmapped_space_after = iter->unmapped_space_after - range.size;
    iter->unmapped_space_after = 0;

    mappings_.insert(++iter, range);
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
    LOG(INFO) << " real_addr: " << std::hex << it->real_addr
              << " mapped: " << std::hex << it->mapped_addr
              << " id: " << std::hex << it->id
              << " size: " << std::hex << it->size;
  }
}

bool AddressMapper::GetMappedAddress(const uint64_t real_addr,
                                     uint64_t* mapped_addr) const {
  CHECK(mapped_addr);
  MappingList::const_iterator iter;
  for (iter = mappings_.begin(); iter != mappings_.end(); ++iter) {
    if (!iter->ContainsAddress(real_addr))
      continue;
    *mapped_addr = iter->mapped_addr + real_addr - iter->real_addr;
    return true;
  }
  return false;
}

bool AddressMapper::GetMappedIDAndOffset(const uint64_t real_addr,
                                         uint64_t* id,
                                         uint64_t* offset) const {
  CHECK(id);
  CHECK(offset);
  MappingList::const_iterator iter;
  for (iter = mappings_.begin(); iter != mappings_.end(); ++iter) {
    if (!iter->ContainsAddress(real_addr))
      continue;
    *id = iter->id;
    *offset = real_addr - iter->real_addr + iter->offset_base;
    return true;
  }
  return false;
}

uint64_t AddressMapper::GetMaxMappedLength() const {
  if (IsEmpty())
    return 0;

  uint64_t min = mappings_.begin()->mapped_addr;

  MappingList::const_iterator iter = mappings_.end();
  --iter;
  uint64_t max = iter->mapped_addr + iter->size;

  return max - min;
}

bool AddressMapper::Unmap(const MappedRange& range) {
  MappingList::iterator iter;
  // TODO(sque): this is highly inefficient since Unmap() is called from a
  // function that has already iterated to the right place within |mappings_|.
  // For a first revision, I am sacrificing efficiency for of clarity, due to
  // the trickiness of removing elements using iterators.
  for (iter = mappings_.begin(); iter != mappings_.end(); ++iter) {
    if (range.real_addr == iter->real_addr && range.size == iter->size) {
      // Add the freed up space to the free space counter of the previous
      // mapped region, if it exists.
      if (iter != mappings_.begin()) {
        --iter;
        iter->unmapped_space_after += range.size + range.unmapped_space_after;
        ++iter;
      }
      mappings_.erase(iter);
      return true;
    }
  }
  return false;
}

}  // namespace quipper
