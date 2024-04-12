#include "llvm_propeller_perf_branch_frequencies_aggregator.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "branch_frequencies.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_statistics.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/testing/status_matchers.h"

namespace devtools_crosstool_autofdo {
namespace {
using ::testing::AllOf;
using ::testing::Field;
using ::testing::SizeIs;
using ::testing::status::IsOkAndHolds;
using ::testing::status::StatusIs;

class MockPerfDataProvider : public PerfDataProvider {
 public:
  MOCK_METHOD(absl::StatusOr<std::optional<PerfDataProvider::BufferHandle>>,
              GetNext, ());
  MOCK_METHOD(absl::StatusOr<std::vector<PerfDataProvider::BufferHandle>>,
              GetAllAvailableOrNext, ());
};

static constexpr std::string_view kTestDataDir = "/testdata/";

TEST(PerfBranchFrequenciesAggregator, FailsIfNoPerfData) {
  auto mock_perf_data_provider = std::make_unique<MockPerfDataProvider>();

  // PerfDataProvider::BufferHandle is not copyable, so we can't use
  // `testing::Return` here.
  EXPECT_CALL(*mock_perf_data_provider, GetNext()).WillRepeatedly([]() {
    return absl::InvalidArgumentError("No perf data");
  });
  EXPECT_CALL(*mock_perf_data_provider, GetAllAvailableOrNext())
      .WillRepeatedly(
          []() { return absl::InvalidArgumentError("No perf data"); });

  PropellerStats stats;
  EXPECT_THAT(
      PerfBranchFrequenciesAggregator(std::move(mock_perf_data_provider))
          .AggregateBranchFrequencies(PropellerOptions(), BinaryContent(),
                                      stats),
      StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(PerfBranchFrequenciesAggregator, SelectsMmapForProfiledBinary) {
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryContent> binary_content,
      GetBinaryContent(absl::StrCat(::testing::SrcDir(), kTestDataDir, "propeller_sample.arm.bin")));

  PropellerStats stats;
  EXPECT_THAT(
      PerfBranchFrequenciesAggregator(
          std::make_unique<GenericFilePerfDataProvider>(std::vector{
              absl::StrCat(::testing::SrcDir(), kTestDataDir, "propeller_sample.arm.perfdata")}))
          .AggregateBranchFrequencies(
              PropellerOptionsBuilder().SetProfiledBinaryName(
                  "nonexistent_binary"),
              *binary_content, stats),
      StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST(PerfBranchFrequenciesAggregator, SelectsMmapForBinaryName) {
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryContent> binary_content,
      GetBinaryContent(absl::StrCat(::testing::SrcDir(), kTestDataDir, "propeller_sample.arm.bin")));

  PropellerStats stats;
  EXPECT_THAT(
      PerfBranchFrequenciesAggregator(
          std::make_unique<GenericFilePerfDataProvider>(std::vector{
              absl::StrCat(::testing::SrcDir(), kTestDataDir, "propeller_sample.arm.perfdata")}))
          .AggregateBranchFrequencies(PropellerOptionsBuilder()
                                          .SetBinaryName("nonexistent_binary")
                                          .SetIgnoreBuildId(true),
                                      *binary_content, stats),
      StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST(PerfBranchFrequenciesAggregator, AggregatesPerfFrequencyData) {
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryContent> binary_content,
      GetBinaryContent(absl::StrCat(::testing::SrcDir(), kTestDataDir, "propeller_sample.arm.bin")));

  PropellerStats stats;
  EXPECT_THAT(
      PerfBranchFrequenciesAggregator(
          std::make_unique<GenericFilePerfDataProvider>(std::vector{
              absl::StrCat(::testing::SrcDir(), kTestDataDir, "propeller_sample.arm.perfdata")}))
          .AggregateBranchFrequencies(PropellerOptionsBuilder().SetBinaryName(
                                          "propeller_sample.arm.perfdata"),
                                      *binary_content, stats),
      IsOkAndHolds(AllOf(
          Field("taken_branch_counters",
                &BranchFrequencies::taken_branch_counters, SizeIs(27)),
          Field("not_taken_branch_counters",
                &BranchFrequencies::not_taken_branch_counters, SizeIs(11)))));

  EXPECT_EQ(stats.profile_stats.br_counters_accumulated, 807195);
}

}  // namespace
}  // namespace devtools_crosstool_autofdo
