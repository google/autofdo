#include "perfdata_reader.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "lbr_aggregation.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_perf_data_provider.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/match.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "status_matchers.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace devtools_crosstool_autofdo {
#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::HasSubstr;
using ::testing::Optional;
using ::testing::SizeIs;

// propeller_sample_with_two_same_binaries.perfdata contains buildid list:
//   04e6da50a63d4b859b0be7e235937cd5a7996ecf .../propeller_sample_1.bin
//   04e6da50a63d4b859b0be7e235937cd5a7996ecf .../propeller_sample_2.bin
TEST(PerfdataReaderTest, SelectBinaryInfo_duplicated_binaries) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_old.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_with_two_same_binaries.perfdata");
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(binary));
  EXPECT_EQ(binary_content->build_id,
            "04e6da50a63d4b859b0be7e235937cd5a7996ecf");

  FilePerfDataProvider provider({perfdata});
  absl::StatusOr<std::optional<PerfDataProvider::BufferHandle>> buffer =
      provider.GetNext();
  EXPECT_THAT(buffer, IsOkAndHolds(Optional(_)));
  ASSERT_OK_AND_ASSIGN(
      PerfDataReader perf_data_reader,
      BuildPerfDataReader(**std::move(buffer), binary_content.get(),
                          /*match_mmap_name=*/""));

  EXPECT_THAT(perf_data_reader.binary_mmaps(), SizeIs(2));
  auto &mmap_set1 = perf_data_reader.binary_mmaps().begin()->second;
  auto &mmap_set2 = perf_data_reader.binary_mmaps().rbegin()->second;
  std::string fn1 = mmap_set1.begin()->file_name;
  std::string fn2 = mmap_set2.begin()->file_name;
  EXPECT_TRUE((absl::StrContains(fn1, "propeller_sample_1.bin") &&
               absl::StrContains(fn2, "propeller_sample_2.bin")) ||
              (absl::StrContains(fn1, "propeller_sample_2.bin") &&
               absl::StrContains(fn2, "propeller_sample_1.bin")));
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
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(binary));
  EXPECT_TRUE(binary_content->is_pie);
  // Only 1 loadable & executable segment.
  EXPECT_THAT(binary_content->segments,
              ElementsAre(FieldsAre(0x580, 0x1580, 0x1a0)));

  FilePerfDataProvider provider({perfdata});
  absl::StatusOr<std::optional<PerfDataProvider::BufferHandle>> buffer =
      provider.GetNext();
  EXPECT_THAT(buffer, IsOkAndHolds(Optional(_)));
  ASSERT_OK_AND_ASSIGN(
      PerfDataReader perfdata_reader,
      BuildPerfDataReader(**std::move(buffer), binary_content.get(),
                          /*match_mmap_name=*/""));
  EXPECT_THAT(perfdata_reader.binary_mmaps(), SizeIs(1));

  // Make sure mmap's offset does not equals to segment's offset before
  // proceeding to the following test. (This is the sole purpose that we
  // introduced BinaryPerfInfo data structure.
  EXPECT_NE(
      binary_content->segments.begin()->offset,
      (*(*perfdata_reader.binary_mmaps().begin()).second.begin()).page_offset);

  // We know by readelf -Ws "foo"'s symbol address = 0x1640
  uint64_t foo_sym_addr = 0x1640;
  // We know one cycle event:
  //    902132 7fedfd0306a0 foo+0x60 (libro_sample.so)
  // We translate it:
  uint64_t addr =
      perfdata_reader.RuntimeAddressToBinaryAddress(902132, 0x7fedfd0306a0);
  EXPECT_EQ(addr, foo_sym_addr + 0x60);
}

TEST(PerfReaderTest, DifferentBuildIdWithProfiledBinaryNamePass) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_different_buildid.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(binary));

  FilePerfDataProvider provider({perfdata});

  absl::StatusOr<std::optional<PerfDataProvider::BufferHandle>> buffer =
      provider.GetNext();
  ASSERT_THAT(buffer, IsOkAndHolds(Optional(_)));

  // perf.data has a file with named "propeller_sample.bin.gen".
  EXPECT_OK(
      BuildPerfDataReader(**std::move(buffer), binary_content.get(),
                          /*match_mmap_name=*/"propeller_sample.bin.gen"));
}

TEST(PerfReaderTest, AggregateLbr) {
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(binary));

  FilePerfDataProvider provider({perfdata});
  ASSERT_OK_AND_ASSIGN(std::optional<PerfDataProvider::BufferHandle> buffer,
                       provider.GetNext());

  ASSERT_OK_AND_ASSIGN(
      PerfDataReader perf_data_reader,
      BuildPerfDataReader(std::move(buffer.value()), binary_content.get(),
                          /*match_mmap_name=*/""));

  EXPECT_THAT(perf_data_reader.binary_mmaps(), SizeIs(3));
  // TODO(b/160191690): Should this test check that all instaces are loaded
  // in different places?
  // All instances of the same pie binary cannot be loaded into same place.
  bool all_equals = true;
  for (auto i = perf_data_reader.binary_mmaps().begin(),
            e = std::prev(perf_data_reader.binary_mmaps().end());
       i != e; ++i)
    all_equals &= (i->second == std::next(i)->second);
  EXPECT_FALSE(all_equals);

  LbrAggregation lbr_aggregation;
  perf_data_reader.AggregateLBR(&lbr_aggregation);
  EXPECT_THAT(lbr_aggregation.branch_counters, SizeIs(27));
  EXPECT_THAT(lbr_aggregation.fallthrough_counters, SizeIs(30));
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
