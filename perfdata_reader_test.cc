#include "perfdata_reader.h"

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/match.h"
#include "third_party/abseil/absl/strings/str_cat.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

TEST(PerfdataReaderTest, SelectBinaryInfo_buildid) {
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

// propeller_sample_with_two_same_binaries.perfdata contains buildid list:
//   04e6da50a63d4b859b0be7e235937cd5a7996ecf .../propeller_sample_1.bin
//   04e6da50a63d4b859b0be7e235937cd5a7996ecf .../propeller_sample_2.bin
TEST(PerfdataReaderTest, SelectBinaryInfo_duplicated_binaries) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_with_two_same_binaries.perfdata");
  auto reader = devtools_crosstool_autofdo::PerfDataReader();
  devtools_crosstool_autofdo::BinaryPerfInfo bpi;
  devtools_crosstool_autofdo::BinaryInfo &binary_info = bpi.binary_info;
  reader.SelectBinaryInfo(binary, &binary_info);
  EXPECT_STREQ(binary_info.build_id.c_str(),
               "04e6da50a63d4b859b0be7e235937cd5a7996ecf");
  EXPECT_TRUE(reader.SelectPerfInfo(perfdata, "", &bpi));

  EXPECT_EQ(bpi.binary_mmaps.size(), 2);
  auto &mmap_set1 = bpi.binary_mmaps.begin()->second;
  auto &mmap_set2 = bpi.binary_mmaps.rbegin()->second;
  std::string fn1 = mmap_set1.begin()->file_name;
  std::string fn2 = mmap_set2.begin()->file_name;
  EXPECT_TRUE((absl::StrContains(fn1, "propeller_sample_1.bin") &&
               absl::StrContains(fn2, "propeller_sample_2.bin")) ||
              (absl::StrContains(fn1, "propeller_sample_2.bin") &&
               absl::StrContains(fn2, "propeller_sample_1.bin")));
}

TEST(PerfdataReaderTest, PieAndNoBuildId) {
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

TEST(PerfdataReaderTest, TestSharedLibrary) {
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
