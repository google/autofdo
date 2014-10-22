// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "address_mapper.h"

#include <algorithm>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"

#include "quipper_test.h"

namespace {

struct Range {
  uint64_t addr;
  uint64_t size;
  uint64_t id;
  uint64_t base_offset;

  Range() : addr(0), size(0), id(kuint64max), base_offset(0) {}

  Range(uint64_t addr, uint64_t size)
      : addr(addr), size(size), id(kuint64max), base_offset(0) {}

  Range(uint64_t addr, uint64_t size, uint64_t id)
      : addr(addr), size(size), id(id), base_offset(0) {}

  Range(uint64_t addr, uint64_t size, uint64_t id, uint64_t base_offset)
      : addr(addr), size(size), id(id), base_offset(base_offset) {}

  bool contains(const uint64_t check_addr) const {
    return (check_addr >= addr && check_addr < addr + size);
  }
};

// Some address ranges to map.  It is important that none of these overlap with
// each other, nor are any two of them contiguous.
const Range kMapRanges[] = {
  Range(0xff000000,   0x100000, 0xdeadbeef),
  Range(0x00a00000,    0x10000, 0xcafebabe),
  Range(0x0c000000,  0x1000000, 0x900df00d),
  Range(0x00001000,    0x30000, 0x9000091e),
};

// List of real addresses that are not in the above ranges.
const uint64_t kAddressesNotInRanges[] = {
  0x00000000,
  0x00000100,
  0x00038000,
  0x00088888,
  0x00100000,
  0x004fffff,
  0x00a20000,
  0x00cc0000,
  0x00ffffff,
  0x03e00000,
  0x0b000000,
  0x0d100000,
  0x0fffffff,
  0x1fffffff,
  0x7ffffff0,
  0xdffffff0,
  0xfe000000,
  0xffffffff,
};

// A huge region that overlaps with all ranges in |kMapRanges|.
const Range kBigRegion = Range(0xa00, 0xff000000);

// A region that extends to the end of the address space.
const Range kEndRegion = Range(0xffffffff00000000, 0x100000000);

// A region toward the end of address space that overruns the end of the address
// space.
const Range kOutOfBoundsRegion = Range(0xffffffff00000000, 0x200000000);

// A huge region that covers all of the available space.
const Range kFullRegion = Range(0, kuint64max);

// Number of regularly-spaced intervals within a mapped range to test.
const int kNumRangeTestIntervals = 8;

// The default range ID when it is not explicitly defined during a mapping.
const uint64_t kUndefinedRangeID = kuint64max;

// A simple test function to convert a real address to a mapped address.
// Address ranges in |ranges| are mapped starting at address 0.
uint64_t GetMappedAddressFromRanges(const Range* ranges,
                                    const unsigned int num_ranges,
                                    const uint64_t addr) {
  unsigned int i;
  uint64_t mapped_range_addr;
  for (i = 0, mapped_range_addr = 0;
       i < num_ranges;
       mapped_range_addr += ranges[i].size, ++i) {
    const Range& range = ranges[i];
    if (range.contains(addr))
      return (addr - range.addr) + mapped_range_addr;
  }
  return static_cast<uint64_t>(-1);
}

}  // namespace

using quipper::AddressMapper;

// The unit test class for AddressMapper.
class AddressMapperTest : public ::testing::Test {
 public:
  AddressMapperTest() {}
  ~AddressMapperTest() {}

  virtual void SetUp() {
    mapper_.reset(new AddressMapper);
  }

 protected:
  // Maps a range using the AddressMapper and makes sure that it was successful.
  // Does not specify a range ID.
  bool MapRange(const Range& range, bool remove_old_mappings) {
    LOG(INFO) << "Mapping range at " << std::hex << range.addr
              << " with length of " << std::hex << range.size;
    return mapper_->Map(range.addr, range.size, remove_old_mappings);
  }

  // Maps a range using the AddressMapper and makes sure that it was successful.
  // Provides a specific range ID.
  bool MapRangeWithID(const Range& range, bool remove_old_mappings) {
    LOG(INFO) << "Mapping range at " << std::hex << range.addr
              << " with length of " << range.size << " and id " << range.id;
    return mapper_->MapWithID(range.addr, range.size, range.id,
                              range.base_offset, remove_old_mappings);
  }

  // Tests address mapping without checking for a particular ID.
  void TestMappedRange(const Range& range,
                       const uint64_t expected_mapped_addr) {
    TestMappedRangeWithID(range, expected_mapped_addr, kUndefinedRangeID, 0);
  }

  // Tests a range that has been mapped.  |expected_mapped_addr| is the starting
  // address that it should have been mapped to.  This mapper will test the
  // start and end addresses of the range, as well as a bunch of addresses
  // inside it.  Also checks lookup of ID and offset.
  void TestMappedRangeWithID(const Range& range,
                             const uint64_t expected_mapped_addr,
                             const uint64_t expected_id,
                             const uint64_t expected_base_offset) {
    uint64_t mapped_addr = kuint64max;

    LOG(INFO) << "Testing range at " << std::hex << range.addr
              << " with length of " << std::hex << range.size;

    // Check address at the beginning of the range and at subsequent intervals.
    for (int i = 0; i < kNumRangeTestIntervals; ++i) {
      const uint64_t offset = i * (range.size / kNumRangeTestIntervals);
      uint64_t addr = range.addr + offset;
      EXPECT_TRUE(mapper_->GetMappedAddress(addr, &mapped_addr));
      EXPECT_EQ(expected_mapped_addr + offset, mapped_addr);

      uint64_t mapped_offset;
      uint64_t mapped_id;
      EXPECT_TRUE(
          mapper_->GetMappedIDAndOffset(addr, &mapped_id, &mapped_offset));
      EXPECT_EQ(expected_base_offset + offset, mapped_offset);
      EXPECT_EQ(expected_id, mapped_id);
    }

    // Check address at end of the range.
    EXPECT_TRUE(mapper_->GetMappedAddress(range.addr + range.size - 1,
                                          &mapped_addr));
    EXPECT_EQ(expected_mapped_addr + range.size - 1, mapped_addr);
  }

  scoped_ptr<AddressMapper> mapper_;
};

// Map one range at a time and test looking up addresses.
TEST_F(AddressMapperTest, MapSingle) {
  for (unsigned int i = 0; i < arraysize(kMapRanges); ++i) {
    mapper_.reset(new AddressMapper);
    const Range& range = kMapRanges[i];
    ASSERT_TRUE(MapRange(range, false));
    EXPECT_EQ(1, mapper_->GetNumMappedRanges());
    TestMappedRange(range, 0);

    // Check addresses before the mapped range, should be invalid.
    uint64_t mapped_addr;
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr - 1, &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr - 0x100, &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr + range.size,
                                           &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr + range.size + 0x100,
                                           &mapped_addr));
    EXPECT_EQ(range.size, mapper_->GetMaxMappedLength());
  }
}

// Map all the ranges at once and test looking up addresses.
TEST_F(AddressMapperTest, MapAll) {
  unsigned int i;
  uint64_t size_mapped = 0;
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    ASSERT_TRUE(MapRange(kMapRanges[i], false));
    size_mapped += kMapRanges[i].size;
  }
  EXPECT_EQ(arraysize(kMapRanges), mapper_->GetNumMappedRanges());

  // Check the max mapped length in quipper space.
  EXPECT_EQ(size_mapped, mapper_->GetMaxMappedLength());

  // For each mapped range, test addresses at the start, middle, and end.
  // Also test the address right before and after each range.
  uint64_t mapped_addr;
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    const Range& range = kMapRanges[i];
    TestMappedRange(range,
                    GetMappedAddressFromRanges(kMapRanges,
                                               arraysize(kMapRanges),
                                               range.addr));

    // Check addresses before and after the mapped range, should be invalid.
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr - 1, &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr - 0x100, &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr + range.size,
                                           &mapped_addr));
    EXPECT_FALSE(mapper_->GetMappedAddress(range.addr + range.size + 0x100,
                                           &mapped_addr));
  }

  // Test some addresses that are out of these ranges, should not be able to
  // get mapped addresses.
  for (i = 0; i < arraysize(kAddressesNotInRanges); ++i) {
    EXPECT_FALSE(mapper_->GetMappedAddress(kAddressesNotInRanges[i],
                 &mapped_addr));
  }
}

// Map all the ranges at once and test looking up IDs and offsets.
TEST_F(AddressMapperTest, MapAllWithIDsAndOffsets) {
  unsigned int i;
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    const Range& range = kMapRanges[i];
    LOG(INFO) << "Mapping range at " << std::hex << range.addr
              << " with length of " << std::hex << range.size;
    ASSERT_TRUE(mapper_->MapWithID(kMapRanges[i].addr,
                                   kMapRanges[i].size,
                                   kMapRanges[i].id,
                                   0,
                                   false));
  }
  EXPECT_EQ(arraysize(kMapRanges), mapper_->GetNumMappedRanges());

  // For each mapped range, test addresses at the start, middle, and end.
  // Also test the address right before and after each range.
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    const Range& range = kMapRanges[i];
    TestMappedRangeWithID(range,
                          GetMappedAddressFromRanges(kMapRanges,
                                                     arraysize(kMapRanges),
                                                     range.addr),
                          range.id,
                          0);
  }
}

// Test overlap detection.
TEST_F(AddressMapperTest, OverlapSimple) {
  unsigned int i;
  // Map all the ranges first.
  for (i = 0; i < arraysize(kMapRanges); ++i)
    ASSERT_TRUE(MapRange(kMapRanges[i], false));

  // Attempt to re-map each range, but offset by size / 2.
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    Range range;
    range.addr = kMapRanges[i].addr + kMapRanges[i].size / 2;
    range.size = kMapRanges[i].size;
    // The maps should fail because of overlap with an existing mapping.
    EXPECT_FALSE(MapRange(range, false));
  }

  // Re-map each range with the same offset.  Only this time, remove any old
  // mapped range that overlaps with it.
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    Range range;
    range.addr = kMapRanges[i].addr + kMapRanges[i].size / 2;
    range.size = kMapRanges[i].size;
    EXPECT_TRUE(MapRange(range, true));
    // Make sure the number of ranges is unchanged (one deleted, one added).
    EXPECT_EQ(arraysize(kMapRanges), mapper_->GetNumMappedRanges());

    // The range is shifted in real space but should still be the same in
    // quipper space.
    TestMappedRange(range,
                    GetMappedAddressFromRanges(kMapRanges,
                                               arraysize(kMapRanges),
                                               kMapRanges[i].addr));
  }
}

// Test mapping of a giant map that overlaps with all existing ranges.
TEST_F(AddressMapperTest, OverlapBig) {
  unsigned int i;
  // Map all the ranges first.
  for (i = 0; i < arraysize(kMapRanges); ++i)
    ASSERT_TRUE(MapRange(kMapRanges[i], false));

  // Make sure overlap is detected before removing old ranges.
  ASSERT_FALSE(MapRange(kBigRegion, false));
  ASSERT_TRUE(MapRange(kBigRegion, true));
  EXPECT_EQ(1, mapper_->GetNumMappedRanges());

  TestMappedRange(kBigRegion, 0);

  // Given the list of previously unmapped addresses, test that the ones within
  // |kBigRegion| are now mapped; for the ones that are not, test that they are
  // not mapped.
  for (i = 0; i < arraysize(kAddressesNotInRanges); ++i) {
    uint64_t addr = kAddressesNotInRanges[i];
    uint64_t mapped_addr = kuint64max;
    bool map_success = mapper_->GetMappedAddress(addr, &mapped_addr);
    if (kBigRegion.contains(addr)) {
      EXPECT_TRUE(map_success);
      EXPECT_EQ(addr - kBigRegion.addr, mapped_addr);
    } else {
      EXPECT_FALSE(map_success);
    }
  }

  // Check that addresses in the originally mapped ranges no longer map to the
  // same addresses if they fall within |kBigRegion|, and don't map at all if
  // they are not within |kBigRegion|.
  for (i = 0; i < arraysize(kMapRanges); ++i) {
    const Range& range = kMapRanges[i];
    for (uint64_t addr = range.addr;
         addr < range.addr + range.size;
         addr += range.size / kNumRangeTestIntervals) {
      uint64_t mapped_addr = kuint64max;
      bool map_success = mapper_->GetMappedAddress(addr, &mapped_addr);
      if (kBigRegion.contains(addr)) {
        EXPECT_TRUE(map_success);
        EXPECT_EQ(addr - kBigRegion.addr, mapped_addr);
      } else {
        EXPECT_FALSE(map_success);
      }
    }
  }

  EXPECT_EQ(kBigRegion.size, mapper_->GetMaxMappedLength());
}

// Test a mapping at the end of memory space.
TEST_F(AddressMapperTest, EndOfMemory) {
  ASSERT_TRUE(MapRange(kEndRegion, true));
  EXPECT_EQ(1, mapper_->GetNumMappedRanges());
  TestMappedRange(kEndRegion, 0);
}

// Test mapping of an out-of-bounds mapping.
TEST_F(AddressMapperTest, OutOfBounds) {
  ASSERT_FALSE(MapRange(kOutOfBoundsRegion, false));
  ASSERT_FALSE(MapRange(kOutOfBoundsRegion, true));
  EXPECT_EQ(0, mapper_->GetNumMappedRanges());
  uint64_t mapped_addr;
  EXPECT_FALSE(
      mapper_->GetMappedAddress(kOutOfBoundsRegion.addr + 0x100, &mapped_addr));
}

// Test mapping of a region that covers the entire memory space.  Then map other
// regions over it.
TEST_F(AddressMapperTest, FullRange) {
  ASSERT_TRUE(MapRange(kFullRegion, false));
  int num_expected_ranges = 1;
  EXPECT_EQ(num_expected_ranges, mapper_->GetNumMappedRanges());

  TestMappedRange(kFullRegion, 0);

  // Map some smaller ranges.
  for (size_t i = 0; i < arraysize(kMapRanges); ++i) {
    const Range& range = kMapRanges[i];
    // Check for collision first.
    ASSERT_FALSE(MapRange(range, false));
    ASSERT_TRUE(MapRange(range, true));

    // Make sure the number of mapped ranges has increased by two.  The mapping
    // should have split an existing range.
    num_expected_ranges += 2;
    EXPECT_EQ(num_expected_ranges, mapper_->GetNumMappedRanges());
  }
}

// Test mapping of one range in the middle of an existing range. The existing
// range should be split into two to accommodate it. Also tests the use of base
// offsets.
TEST_F(AddressMapperTest, SplitRangeWithOffsetBase) {
  // Define the two ranges, with distinct IDs.
  const Range kFirstRange(0x10000, 0x4000, 0);
  const Range kSecondRange(0x12000, 0x1000, 1);

  // As a sanity check, make sure the second range is fully contained within the
  // first range.
  EXPECT_LT(kFirstRange.addr, kSecondRange.addr);
  EXPECT_GT(kFirstRange.addr + kFirstRange.size,
            kSecondRange.addr + kSecondRange.size);

  // Map the two ranges.
  ASSERT_TRUE(MapRangeWithID(kFirstRange, true));
  ASSERT_TRUE(MapRangeWithID(kSecondRange, true));

  // The first range should have been split into two parts to make way for the
  // second range. There should be a total of three ranges.
  EXPECT_EQ(3, mapper_->GetNumMappedRanges());

  // Now make sure the mappings are correct.

  // The first range has been split up. Define the expected ranges here.
  const Range kFirstRangeHead(kFirstRange.addr,
                              kSecondRange.addr - kFirstRange.addr,
                              kFirstRange.id);
  const Range kFirstRangeTail(kSecondRange.addr + kSecondRange.size,
                              kFirstRange.addr + kFirstRange.size -
                                  (kSecondRange.addr + kSecondRange.size),
                              kFirstRange.id,
                              kSecondRange.addr + kSecondRange.size);

  // Test the two remaining parts of the first range.
  TestMappedRangeWithID(kFirstRangeHead, 0, kFirstRangeHead.id, 0);
  TestMappedRangeWithID(kFirstRangeTail,
                        kFirstRangeTail.addr - kFirstRangeHead.addr,
                        kFirstRangeTail.id,
                        kFirstRangeTail.addr - kFirstRangeHead.addr);

  // Test the second range normally.
  TestMappedRangeWithID(kSecondRange, kSecondRange.addr - kFirstRange.addr,
                        kSecondRange.id, 0);
}

int main(int argc, char * argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
