#include "spe_sample_reader.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "util/testing/status_matchers.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {
namespace {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::Pair;
using ::testing::SizeIs;
using ::testing::status::StatusIs;

inline constexpr absl::string_view kTestDataDir =
    "//testdata/";

TEST(PerfSpeDataSampleReader, RequiresAArch64) {
  PerfSpeDataSampleReader reader(
      PerfSpeDataSampleReader::BranchIdStrategy::kSampling,
      /*profile_file=*/
      absl::StrCat(testing::SrcDir(), kTestDataDir,
                   "propeller_sample.arm.perfdata"),
      /*mmap_regex=*/".*",
      /*binary_file=*/
      absl::StrCat(testing::SrcDir(), kTestDataDir, "propeller_sample.bin"));

  EXPECT_THAT(
      reader.GenerateAutofdoProfile(absl::StrCat(
          testing::SrcDir(), kTestDataDir, "propeller_sample.arm.perfdata")),
      StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(PerfSpeDataSampleReader, ComputesRangesWithSamplingOnly) {
  PerfSpeDataSampleReader reader(
      PerfSpeDataSampleReader::BranchIdStrategy::kSampling,
      absl::StrCat(testing::SrcDir(), kTestDataDir,
                   "propeller_sample.arm.perfdata"),
      "propeller_sample.arm.bin",
      absl::StrCat(testing::SrcDir(), kTestDataDir,
                   "propeller_sample.arm.bin"));

  EXPECT_TRUE(reader.ReadAndSetTotalCount());
  EXPECT_EQ(reader.GetTotalSampleCount(), 1263256);

  EXPECT_THAT(
      reader.branch_count_map(),
      AllOf(SizeIs(26), Contains(Pair(Pair(0x45748, 0x457d8), 224428))));
  EXPECT_THAT(
      reader.range_count_map(),
      AllOf(SizeIs(32), Contains(Pair(Pair(0x45810, 0x4582c), 247609))));
  EXPECT_EQ(reader.GetTotalCount(), 26734112);
}

TEST(PerfSpeDataSampleReader, ComputesRangesWithDisassembly) {
  PerfSpeDataSampleReader reader(
      PerfSpeDataSampleReader::BranchIdStrategy::kDisassembly,
      absl::StrCat(testing::SrcDir(), kTestDataDir,
                   "propeller_sample.arm.perfdata"),
      "propeller_sample.arm.bin",
      absl::StrCat(testing::SrcDir(), kTestDataDir,
                   "propeller_sample.arm.bin"));

  EXPECT_TRUE(reader.ReadAndSetTotalCount());
  EXPECT_EQ(reader.GetTotalSampleCount(), 1263251);

  EXPECT_THAT(
      reader.range_count_map(),
      AllOf(SizeIs(30), Contains(Pair(Pair(0x45810, 0x4582c), 247609))));
  EXPECT_EQ(reader.GetTotalCount(), 26309075);
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
