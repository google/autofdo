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

ABSL_DECLARE_FLAG(uint64_t, strip_dup_backedge_stride_limit);

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

class SampleReaderTest : public testing::Test {
 protected:
  static const char kTestDataDir[];

  SampleReaderTest() {
    absl::SetFlag(&FLAGS_strip_dup_backedge_stride_limit, 0x50);
  }
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
  EXPECT_TRUE(writer.Write(nullptr));

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

TEST_F(SampleReaderTest, ReadKernelKallsymsProfile) {
  // Verify that the perf reader counts branches mapped to
  // [kernel.kallsyms]_stext when the kernel dso name is [kernel.kallsyms].

  // $ perf buildid-list -i testdata/perf-kernel.data
  // d4eba24dde8ec63cbdf519e6b4008c4ecdcf1f49 [kernel.kallsyms]
  // $ perf report -D -i testdata/perf-kernel.data | grep kallsyms
  // 0 0 0x1000 [0x60]: PERF_RECORD_MMAP -1/0: [0xffffffe43f680800(0xb7f800) @
  // 0xffffffe43f680800]: x [kernel.kallsyms]_stext
  std::string profile =
      FLAGS_test_srcdir + kTestDataDir + "perf-kernel.data";
  devtools_crosstool_autofdo::PerfDataSampleReader reader(
      profile, ".*/vmlinux", "d4eba24dde8ec63cbdf519e6b4008c4ecdcf1f49");
  ASSERT_TRUE(reader.ReadAndSetTotalCount());
  EXPECT_EQ(reader.GetTotalSampleCount(), 1421);
}

TEST_F(SampleReaderTest, ReadVmlinuxProfile) {
  // Verify that the perf reader counts branches mapped to
  // [kernel.kallsyms]_stext when the kernel dso name is /tmp/vmlinux.

  // $ perf buildid-list -i \
  //   devtools/crosstool/autofdo/testdata/perf-vmlinux.data
  // 948da3c05fff6a515eab7b9dd416e30564b2ccf2 /tmp/vmlinux
  std::string profile =
      FLAGS_test_srcdir + kTestDataDir + "perf-vmlinux.data";
  devtools_crosstool_autofdo::PerfDataSampleReader reader(
      profile, ".*/vmlinux", "948da3c05fff6a515eab7b9dd416e30564b2ccf2");
  ASSERT_TRUE(reader.ReadAndSetTotalCount());
  EXPECT_EQ(reader.GetTotalSampleCount(), 1936);
}
}  // namespace
