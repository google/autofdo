// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// These tests check to see that parsing the various profile data
// formats work properly.  The "magic" numbers in the various checks
// come from the addresses collected during test profiles of the
// test.binary application in the testdata directory.

#include "sample_reader.h"

#include <utility>

#include "base/commandlineflags.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/declare.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/str_cat.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

ABSL_DECLARE_FLAG(uint64_t, strip_dup_backedge_stride_limit);

namespace {

class SampleReaderTest : public testing::Test {
 protected:
  static const char kTestDataDir[];

  SampleReaderTest() { absl::SetFlag(&FLAGS_strip_dup_backedge_stride_limit, 0x50); }
};

const char SampleReaderTest::kTestDataDir[] =
  "/testdata/";

TEST_F(SampleReaderTest, ReadPerf) {
  devtools_crosstool_autofdo::PerfDataSampleReader reader(
      FLAGS_test_srcdir + kTestDataDir + "test.perf",
      ".*/gzip_base.intel90-linux", "");
  ASSERT_TRUE(reader.ReadAndSetTotalCount());

  EXPECT_EQ(reader.GetSampleCountOrZero(0xfe0), 238);
  EXPECT_EQ(reader.GetSampleCountOrZero(0x1005), 87);
  EXPECT_EQ(reader.GetTotalCount(), 79874);
}

TEST_F(SampleReaderTest, ReadLBR) {
  devtools_crosstool_autofdo::PerfDataSampleReader reader(
      FLAGS_test_srcdir + kTestDataDir + "test.lbr",
      "test.binary", "");
  ASSERT_TRUE(reader.ReadAndSetTotalCount());

  EXPECT_EQ(reader.GetSampleCountOrZero(0xfe0), 55);
  EXPECT_EQ(reader.GetSampleCountOrZero(0x1005), 18);
  EXPECT_EQ(reader.GetTotalSampleCount(), 134622);

  const devtools_crosstool_autofdo::RangeCountMap &range_map =
      reader.range_count_map();
  EXPECT_EQ(range_map.size(), 357);

  devtools_crosstool_autofdo::Range range(0x4e80, 0x4e8e);
  EXPECT_NE(range_map.find(range), range_map.end());
  EXPECT_EQ(range_map.find(range)->second, 1134);
  EXPECT_EQ(reader.GetTotalCount(), 5383657);
}

TEST_F(SampleReaderTest, ReadText) {
  devtools_crosstool_autofdo::PerfDataSampleReader lbr_reader(
      FLAGS_test_srcdir + kTestDataDir + "test.lbr",
      "test.binary", "");
  ASSERT_TRUE(lbr_reader.ReadAndSetTotalCount());

  devtools_crosstool_autofdo::TextSampleReaderWriter writer(
      FLAGS_test_tmpdir + "test.txt");
  writer.Merge(lbr_reader);
  EXPECT_EQ(writer.GetSampleCountOrZero(0xfe0), 55);
  EXPECT_TRUE(writer.Write(NULL));

  devtools_crosstool_autofdo::TextSampleReaderWriter reader(
      FLAGS_test_tmpdir + "test.txt");
  ASSERT_TRUE(reader.ReadAndSetTotalCount());
  EXPECT_EQ(reader.GetSampleCountOrZero(0x1005), 18);
  EXPECT_EQ(reader.GetTotalCount(), 5383657);
}

TEST_F(SampleReaderTest, ReadLBRWithDupEntries) {
  devtools_crosstool_autofdo::PerfDataSampleReader reader(
      FLAGS_test_srcdir + kTestDataDir + "dup.lbr", "dup.binary",
      "");
  ASSERT_TRUE(reader.ReadAndSetTotalCount());

  EXPECT_EQ(reader.GetTotalSampleCount(), 327191);

  const devtools_crosstool_autofdo::RangeCountMap &range_map =
      reader.range_count_map();

  // Expect sampleReader to filter out duplicated LBR entries
  // on skylake and won't generate bogus range.
  devtools_crosstool_autofdo::Range range1(0x600, 0x698);
  EXPECT_EQ(range_map.find(range1), range_map.end());
  devtools_crosstool_autofdo::Range range2(0x630, 0x726);
  EXPECT_EQ(range_map.find(range2), range_map.end());
}

TEST_F(SampleReaderTest, SelectBinaryInfo_buildid) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "llvm_function_samples.binary");
  auto reader = devtools_crosstool_autofdo::PerfDataReader();
  devtools_crosstool_autofdo::BinaryInfo binary_info;
  reader.SelectBinaryInfo(binary, &binary_info);
  EXPECT_STREQ(binary_info.build_id.c_str(),
               "a56e4274b3adf7c87d165ca6deb66db002e72e1e");
}

TEST_F(SampleReaderTest, PieAndNoBuildId) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_barebone_pie_nobuildid.bin");
  auto reader = devtools_crosstool_autofdo::PerfDataReader();
  devtools_crosstool_autofdo::BinaryInfo binary_info;
  reader.SelectBinaryInfo(binary, &binary_info);
  EXPECT_TRUE(binary_info.is_pie);
  EXPECT_TRUE(binary_info.build_id.empty());
}

TEST_F(SampleReaderTest, TestSharedLibrary) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "libro_sample.so");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "ro_sample.perf");
  auto reader = devtools_crosstool_autofdo::PerfDataReader();
  devtools_crosstool_autofdo::BinaryPerfInfo binary_perf_info;
  devtools_crosstool_autofdo::BinaryInfo &binary_info =
      binary_perf_info.binary_info;
  reader.SelectBinaryInfo(binary, &binary_info);
  EXPECT_TRUE(binary_info.is_pie);
  // Only 1 loadable & executable segment.
  EXPECT_EQ(binary_info.segments.size(), 1);

  const devtools_crosstool_autofdo::BinaryInfo::Segment &seg =
      binary_info.segments[0];
  EXPECT_EQ(seg.offset, 0x580);
  EXPECT_EQ(seg.vaddr, 0x1580);
  EXPECT_EQ(seg.memsz, 0x1a0);

  EXPECT_TRUE(reader.SelectPerfInfo(perfdata, "", &binary_perf_info));
  EXPECT_EQ(binary_perf_info.binary_mmaps.size(), 1);

  // Make sure mmap's offset does not equals to segment's offset before
  // proceeding to the following test. (This is the sole purpose that we
  // introduced BinaryPerfInfo data structure.
  EXPECT_NE(
      seg.offset,
      (*(*binary_perf_info.binary_mmaps.begin()).second.begin()).page_offset);

  // We know by readelf -Ws "foo"'s symbol address = 0x1640
  uint64_t foo_sym_addr = 0x1640;
  // We know one cycle event:
  //    902132 7fedfd0306a0 foo+0x60 (libro_sample.so)
  // We translate it:
  uint64_t addr = reader.RuntimeAddressToBinaryAddress(902132, 0x7fedfd0306a0,
                                                       binary_perf_info);
  EXPECT_EQ(addr, foo_sym_addr + 0x60);
}

}  // namespace
