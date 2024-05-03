#include "llvm_propeller_profile_generator.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_statistics.h"
#include "llvm_propeller_telemetry_reporter.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "util/testing/status_matchers.h"

namespace devtools_crosstool_autofdo {
namespace {

std::string GetTestDataDirectoryPath() {
  return absl::StrCat(::testing::SrcDir(), "/testdata/");
}

// Returns the content of the file stored in `file_name` as a string.
std::string GetFileContent(absl::string_view file_name) {
  std::ifstream ifs(std::string(file_name), std::ifstream::binary);
  if (!ifs) return "";
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

using ::testing::Eq;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::Ne;
using ::testing::Not;
using ::testing::status::IsOk;
using ::testing::status::StatusIs;

using ::testing::TestWithParam;

struct GeneratePropellerProfileTestCase {
  std::string test_name;
  PropellerOptions propeller_options;
  std::string expected_cc_profile_path;
  std::string expected_ld_profile_path;
};

using GeneratePropellerProfileTest =
    TestWithParam<GeneratePropellerProfileTestCase>;

TEST_P(GeneratePropellerProfileTest, TestGeneratePropellerProfile) {
  const GeneratePropellerProfileTestCase& test_case = GetParam();
  EXPECT_TRUE(!test_case.expected_cc_profile_path.empty() ||
              !test_case.expected_ld_profile_path.empty());
  std::string cc_directives_path =
      absl::StrCat(::testing::TempDir(), "/cc_directives.txt");
  std::string ld_directives_path =
      absl::StrCat(::testing::TempDir(), "/ld_directives.txt");

  PropellerOptions options =
      PropellerOptionsBuilder(test_case.propeller_options)
          .SetClusterOutName(cc_directives_path)
          .SetSymbolOrderOutName(ld_directives_path);
  EXPECT_OK(GeneratePropellerProfiles(options));

  if (!test_case.expected_cc_profile_path.empty()) {
    EXPECT_EQ(GetFileContent(cc_directives_path),
              GetFileContent(test_case.expected_cc_profile_path));
  }
  if (!test_case.expected_ld_profile_path.empty()) {
    EXPECT_EQ(GetFileContent(ld_directives_path),
              GetFileContent(test_case.expected_ld_profile_path));
  }
}

INSTANTIATE_TEST_SUITE_P(
    GeneratePropellerProfileTests, GeneratePropellerProfileTest,
    testing::ValuesIn<GeneratePropellerProfileTestCase>({
        {"GeneratesPropellerProfiles",
         PropellerOptionsBuilder()
             .SetBinaryName(absl::StrCat(GetTestDataDirectoryPath(),
                                         "propeller_sample.bin"))
             .AddInputProfiles(InputProfileBuilder().SetName(absl::StrCat(
                 GetTestDataDirectoryPath(), "propeller_sample.perfdata"))),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_cc_directives.golden.txt"),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_ld_directives.golden.txt")},
        {"GeneratesSplitOnlyProfiles",
         PropellerOptionsBuilder()
             .SetBinaryName(absl::StrCat(GetTestDataDirectoryPath(),
                                         "propeller_sample.bin"))
             .AddInputProfiles(InputProfileBuilder().SetName(absl::StrCat(
                 GetTestDataDirectoryPath(), "propeller_sample.perfdata")))
             .SetCodeLayoutParamsReorderHotBlocks(false),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_split_only_cc_directives.golden.txt"),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_split_only_ld_directives.golden.txt")},
        {"VerboseClusterOutput",
         PropellerOptionsBuilder()
             .SetBinaryName(absl::StrCat(GetTestDataDirectoryPath(),
                                         "propeller_sample.bin"))
             .AddInputProfiles(InputProfileBuilder().SetName(absl::StrCat(
                 GetTestDataDirectoryPath(), "propeller_sample.perfdata")))
             .SetVerboseClusterOutput(true),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_verbose_cc_directives.golden.txt"),
         ""},
        {"HandlesFunctionsInNonDotTextSections",
         PropellerOptionsBuilder()
             .SetBinaryName(absl::StrCat(GetTestDataDirectoryPath(),
                                         "propeller_sample_section.bin"))
             .AddInputProfiles(InputProfileBuilder().SetName(
                 absl::StrCat(GetTestDataDirectoryPath(),
                              "propeller_sample_section.perfdata")))
             .SetFilterNonTextFunctions(false),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_section_cc_directives.golden.txt"),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_section_ld_directives.golden.txt")},
        {"GeneratesPropellerProfilesV0",
         PropellerOptionsBuilder()
             .SetClusterOutVersion(VERSION_0)
             .SetBinaryName(absl::StrCat(GetTestDataDirectoryPath(),
                                         "propeller_sample.bin"))
             .AddInputProfiles(InputProfileBuilder().SetName(absl::StrCat(
                 GetTestDataDirectoryPath(), "propeller_sample.perfdata"))),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_cc_directives.v0.golden.txt"),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_ld_directives.golden.txt")},
        {"GeneratesPropellerProfilesArm",
         PropellerOptionsBuilder()
             .SetBinaryName(absl::StrCat(GetTestDataDirectoryPath(),
                                         "propeller_sample.arm.bin"))
             .AddInputProfiles(
                 InputProfileBuilder()
                     .SetName(absl::StrCat(GetTestDataDirectoryPath(),
                                           "propeller_sample.arm.perfdata"))
                     .SetType(ProfileType::PERF_SPE)),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_cc_directives.arm.golden.txt"),
         absl::StrCat(GetTestDataDirectoryPath(),
                      "propeller_sample_ld_directives.arm.golden.txt")},
    }),
    [](const testing::TestParamInfo<GeneratePropellerProfileTest::ParamType>&
           info) { return info.param.test_name; });

TEST(LlvmPropellerProfileGeneratorTest, ParsePerf0_relative_path) {
  const std::string binary =
      absl::StrCat(GetTestDataDirectoryPath(), "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(GetTestDataDirectoryPath(), "propeller_sample.perfdata");
  // Test that specifying --mmap_name with relative name works.
  // When relative file path is passed to "--binary", we use the name portion of
  // the path (like `basename <filename>`) to match mmap entries. Since
  // propeller_sample.perfdata contains mmaps with file name
  // "propeller_sample.tmp.bin", profile should be generated.
  EXPECT_OK(GeneratePropellerProfiles(
      PropellerOptionsBuilder()
          .SetBinaryName(binary)
          .AddInputProfiles(InputProfileBuilder().SetName(perfdata))
          .SetClusterOutName("dummy.out")
          .SetProfiledBinaryName(
              "any_relative_path/propeller_sample.bin.gen")));
}

TEST(LlvmPropellerProfileGeneratorTest, ParsePerf0_absolute_path) {
  const std::string binary =
      absl::StrCat(GetTestDataDirectoryPath(), "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(GetTestDataDirectoryPath(), "propeller_sample.perfdata");
  EXPECT_THAT(GeneratePropellerProfiles(
                  PropellerOptionsBuilder()
                      .SetBinaryName(binary)
                      .AddInputProfiles(InputProfileBuilder().SetName(perfdata))
                      .SetClusterOutName("dummy.out")
                      .SetProfiledBinaryName(
                          "/any_absolute_path/propeller_sample.bin.gen")),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("No perf file is parsed, cannot proceed.")));
  // When absolute file path is passed to "--binary", we use absolute path to
  // match mmap entries. Since propeller_sample.perfdata only contains mmap with
  // file name "$(RULEDIR)/propeller_sample.bin", this is expected to fail.
}

TEST(GeneratePropellerProfiles, UsesPassedProvider) {
  PropellerOptions options;
  options.set_binary_name(absl::StrCat(::testing::SrcDir(),
                                       "//testdata/"
                                       "propeller_sample.bin"));
  options.set_cluster_out_name(absl::StrCat(
      ::testing::TempDir(),
      "/LlvmPropellerProfileGeneratorTest_UsesPassedProvider_cc_profile.txt"));
  options.set_symbol_order_out_name(absl::StrCat(
      ::testing::TempDir(),
      "/LlvmPropellerProfileGeneratorTest_UsesPassedProvider_ld_profile.txt"));

  // The provider will fail, because the file does not exist, so
  // `GeneratePropellerProfiles` should fail.
  EXPECT_THAT(GeneratePropellerProfiles(
                  options, std::make_unique<GenericFilePerfDataProvider>(
                               std::vector<std::string>{absl::StrCat(
                                   ::testing::SrcDir(),
                                   "//testdata/"
                                   "this_file_does_not_exist.perfdata")})),
              Not(IsOk()));

  // Here the provider will succeed and so should `GeneratePropellerProfiles`.
  EXPECT_OK(GeneratePropellerProfiles(
      options,
      std::make_unique<GenericFilePerfDataProvider>(std::vector<std::string>{
          absl::StrCat(::testing::SrcDir(),
                       "//testdata/"
                       "propeller_sample.perfdata")})));
}

TEST(LlvmPropellerProfileGeneratorTest, ExportsTelemetry) {
  const std::string binary = absl::StrCat(::testing::SrcDir(),
                                          "//testdata/"
                                          "propeller_sample.bin");
  const std::string perfdata = absl::StrCat(::testing::SrcDir(),
                                            "//testdata/"
                                            "propeller_sample.perfdata");

  UnregisterAllPropellerTelemetryReportersForTest();
  testing::MockFunction<void(const BinaryContent&, const PropellerStats&)>
      mock_reporter;
  EXPECT_CALL(
      mock_reporter,
      Call(Field("object_file", &BinaryContent::object_file, Ne(nullptr)),
           Field("cfg_stats", &PropellerStats::cfg_stats,
                 Field("cfgs_created", &PropellerStats::CfgStats::cfgs_created,
                       Eq(2)))))
      .Times(1);
  RegisterPropellerTelemetryReporter(mock_reporter.AsStdFunction());
  EXPECT_OK(GeneratePropellerProfiles(
      PropellerOptionsBuilder()
          .SetBinaryName(binary)
          .AddInputProfiles(InputProfileBuilder().SetName(perfdata))
          .SetClusterOutName("dummy.out")));
  UnregisterAllPropellerTelemetryReportersForTest();
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
