#include "llvm_propeller_perf_lbr_aggregator.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_statistics.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "util/testing/status_matchers.h"

namespace devtools_crosstool_autofdo {
namespace {

static std::string GetAutoFdoTestDataFilePath(absl::string_view filename) {
  return absl::StrCat(::testing::SrcDir(),
                      "/testdata/",
                      filename);
}

TEST(LlvmPropellerProfileComputerTest, TestCheckLbrAggregationOk) {
  const std::string perfdata =
      GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1");

  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample_1.bin"))
          .AddInputProfiles(InputProfileBuilder().SetName(perfdata)));

  PropellerStats stats;
  auto perf_data_provider =
      std::make_unique<GenericFilePerfDataProvider>(std::vector{perfdata});

  auto lbr_aggregator =
      std::make_unique<PerfLbrAggregator>(std::move(perf_data_provider));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(options.binary_name()));

  EXPECT_OK(lbr_aggregator->AggregateLbrData(options, *binary_content, stats));

  EXPECT_EQ(stats.profile_stats.br_counters_accumulated, 119424);
  EXPECT_EQ(stats.disassembly_stats.could_not_disassemble.absolute, 0);
  EXPECT_EQ(stats.disassembly_stats.may_affect_control_flow.absolute, 9);
  EXPECT_EQ(stats.disassembly_stats.cant_affect_control_flow.absolute, 0);
  EXPECT_EQ(stats.disassembly_stats.could_not_disassemble.weighted, 0);
  EXPECT_EQ(stats.disassembly_stats.may_affect_control_flow.weighted, 119419);
  EXPECT_EQ(stats.disassembly_stats.cant_affect_control_flow.weighted, 0);
}

TEST(LlvmPropellerProfileComputerTest, TestCheckLbrAggregationFail) {
  // Providing a binary/perfdata pair that do not match will cause error:
  // "100.00% branches's source addresses failed to disassemble. This indicates
  // an address translation error caused either by a bug in the tool or the
  // provided binary does not match perf.data file."
  const std::string perfdata =
      GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1");
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("ml_test.binary"))
          .SetProfiledBinaryName("propeller_sample_1.bin.gen")
          .AddInputProfiles(InputProfileBuilder().SetName(perfdata)));

  PropellerStats stats;
  auto perf_data_provider =
      std::make_unique<GenericFilePerfDataProvider>(std::vector{perfdata});
  auto lbr_aggregator =
      std::make_unique<PerfLbrAggregator>(std::move(perf_data_provider));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(options.binary_name()));

  EXPECT_OK(lbr_aggregator->AggregateLbrData(options, *binary_content, stats));

  EXPECT_EQ(stats.disassembly_stats.could_not_disassemble.absolute, 9);
  EXPECT_EQ(stats.disassembly_stats.may_affect_control_flow.absolute, 0);
  EXPECT_EQ(stats.disassembly_stats.cant_affect_control_flow.absolute, 0);
  EXPECT_EQ(stats.disassembly_stats.could_not_disassemble.weighted, 119419);
  EXPECT_EQ(stats.disassembly_stats.may_affect_control_flow.weighted, 0);
  EXPECT_EQ(stats.disassembly_stats.cant_affect_control_flow.weighted, 0);
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
