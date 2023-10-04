#include "llvm_propeller_profile_generator.h"

#include <memory>
#include <string>
#include <vector>

#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_statistics.h"
#include "llvm_propeller_telemetry_reporter.h"
#include "file_util.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "status_matchers.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace devtools_crosstool_autofdo {
#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

// Returns the content of the file stored in `file_name` as a string.
std::string GetFileContent(absl::string_view file_name) {
  std::string content;
  CHECK_OK(file::GetContents(file_name, &content));
  return content;
}

using ::testing::Field;
using ::testing::Ne;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::Not;

TEST(LlvmPropellerProfileGeneratorTest, ParsePerf0_relative_path) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  // Test that specifying --mmap_name with relative name works.
  // When relative file path is passed to "--binary", we use the name portion of
  // the path (like `basename <filename>`) to match mmap entries. Since
  // propeller_sample.perfdata contains mmaps with file name
  // "propeller_sample.tmp.bin", profile should be generated.
  EXPECT_OK(GeneratePropellerProfiles(
      PropellerOptionsBuilder()
          .SetBinaryName(binary)
          .AddPerfNames(perfdata)
          .SetClusterOutName("dummy.out")
          .SetProfiledBinaryName(
              "any_relative_path/propeller_sample.bin.gen")));
}

TEST(LlvmPropellerProfileGeneratorTest, NoReorderBlocks) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  const std::string cc_directives =
      absl::StrCat(FLAGS_test_tmpdir, "cc_directives.txt");
  const std::string ld_directives =
      absl::StrCat(FLAGS_test_tmpdir, "ld_directives.txt");
  const std::string cc_directives_golden =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_split_only_cc_directives.golden.txt");
  const std::string ld_directives_golden =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_split_only_ld_directives.golden.txt");
  auto options = PropellerOptionsBuilder()
                     .SetBinaryName(binary)
                     .AddPerfNames(perfdata)
                     .SetClusterOutName(cc_directives)
                     .SetSymbolOrderOutName(ld_directives)
                     .SetCodeLayoutParamsReorderHotBlocks(false);
  EXPECT_OK(GeneratePropellerProfiles(options));
  EXPECT_EQ(GetFileContent(cc_directives),
            GetFileContent(cc_directives_golden));
  EXPECT_EQ(GetFileContent(ld_directives),
            GetFileContent(ld_directives_golden));
}

TEST(LlvmPropellerProfileGeneratorTest, HandlesFunctionsInNonDotTextSections) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_section.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_section.perfdata");
  const std::string cc_directives =
      absl::StrCat(FLAGS_test_tmpdir, "cc_directives.txt");
  const std::string ld_directives =
      absl::StrCat(FLAGS_test_tmpdir, "ld_directives.txt");
  const std::string cc_directives_golden =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_section_cc_directives.golden.txt");
  const std::string ld_directives_golden =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_section_ld_directives.golden.txt");
  PropellerOptions options = PropellerOptionsBuilder()
                                 .SetBinaryName(binary)
                                 .AddPerfNames(perfdata)
                                 .SetClusterOutName(cc_directives)
                                 .SetSymbolOrderOutName(ld_directives)
                                 .SetFilterNonTextFunctions(false);
  EXPECT_OK(GeneratePropellerProfiles(options));
  EXPECT_EQ(GetFileContent(cc_directives),
            GetFileContent(cc_directives_golden));
  EXPECT_EQ(GetFileContent(ld_directives),
            GetFileContent(ld_directives_golden));
}

TEST(LlvmPropellerProfileGeneratorTest, VerboseClusterOutput) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  const std::string cc_directives =
      absl::StrCat(FLAGS_test_tmpdir, "/cc_directives.txt");
  auto options = PropellerOptionsBuilder()
                     .SetBinaryName(binary)
                     .AddPerfNames(perfdata)
                     .SetClusterOutName(cc_directives)
                     .SetVerboseClusterOutput(true);
  EXPECT_OK(GeneratePropellerProfiles(options));

  const std::string cc_directives_golden =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample_verbose_cc_directives.golden.txt");
  EXPECT_EQ(GetFileContent(cc_directives),
            GetFileContent(cc_directives_golden));
}

TEST(GeneratePropellerProfiles, UsesPassedProvider) {
  PropellerOptions options;
  options.set_binary_name(
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin"));
  options.set_cluster_out_name(absl::StrCat(
      FLAGS_test_tmpdir,
      "/LlvmPropellerProfileGeneratorTest_UsesPassedProvider_cc_profile.txt"));
  options.set_symbol_order_out_name(absl::StrCat(
      FLAGS_test_tmpdir,
      "/LlvmPropellerProfileGeneratorTest_UsesPassedProvider_ld_profile.txt"));

  // The provider will fail, because the file does not exist, so
  // `GeneratePropellerProfiles` should fail.
  EXPECT_THAT(
      GeneratePropellerProfiles(
          options,
          std::make_unique<FilePerfDataProvider>(std::vector<std::string>{
              absl::StrCat(FLAGS_test_srcdir,
                           "/testdata/"
                           "this_file_does_not_exist.perfdata")})),
      Not(IsOk()));

  // Here the provider will succeed and so should `GeneratePropellerProfiles`.
  EXPECT_OK(GeneratePropellerProfiles(
      options, std::make_unique<FilePerfDataProvider>(std::vector<std::string>{
                   absl::StrCat(FLAGS_test_srcdir,
                                "/testdata/"
                                "propeller_sample.perfdata")})));
}

TEST(LlvmPropellerProfileGeneratorTest, ExportsTelemetry) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");

  UnregisterAllPropellerTelemetryReportersForTest();
  testing::MockFunction<void(const BinaryContent &, const PropellerStats &)>
      mock_reporter;
  EXPECT_CALL(
      mock_reporter,
      Call(Field("object_file", &BinaryContent::object_file, Ne(nullptr)),
           Field("cfgs_created", &PropellerStats::cfgs_created, Eq(3))))
      .Times(1);
  RegisterPropellerTelemetryReporter(mock_reporter.AsStdFunction());
  EXPECT_OK(GeneratePropellerProfiles(PropellerOptionsBuilder()
                                          .SetBinaryName(binary)
                                          .AddPerfNames(perfdata)
                                          .SetClusterOutName("dummy.out")));
  UnregisterAllPropellerTelemetryReportersForTest();
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
