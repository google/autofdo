#include "llvm_propeller_file_perf_data_provider.h"

#include <fstream>
#include <string>
#include <string_view>

#include "base/logging.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace devtools_crosstool_autofdo {
#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

using ::testing::Eq;
using ::testing::FieldsAre;
using ::testing::HasSubstr;
using ::testing::Optional;
using ::testing::status::IsOkAndHolds;
using ::testing::status::StatusIs;

MATCHER_P(BufferIs, contents_matcher,
          absl::StrCat("an llvm::MemoryBuffer that ",
                       testing::DescribeMatcher<absl::string_view>(
                           contents_matcher, negation))) {
  return testing::ExplainMatchResult(
      contents_matcher, absl::string_view(std::string_view(arg->getBuffer())),
      result_listener);
}

// Writes `contents` to file named `file_name`.
void WriteFile(absl::string_view file_name, absl::string_view contents) {
  std::ofstream stream(file_name, std::ios::binary);
  stream << contents;
  CHECK(!stream.fail());
}

TEST(FilePerfDataProvider, ReadsFilesCorrectly) {
  std::string file1 =
      absl::StrCat(FLAGS_test_tmpdir,
                   "/FilePerfDataProvider_ReadsFilesCorrectly_file1.perf");
  std::string file2 =
      absl::StrCat(FLAGS_test_tmpdir,
                   "/FilePerfDataProvider_ReadsFilesCorrectly_file2.perf");
  WriteFile(file1, "Hello world");
  WriteFile(file2, "Test data");

  FilePerfDataProvider provider({file1, file2});
  EXPECT_THAT(provider.GetNext(),
              IsOkAndHolds(Optional(FieldsAre(absl::StrCat("[1/2] ", file1),
                                              BufferIs("Hello world")))));
  EXPECT_THAT(provider.GetNext(),
              IsOkAndHolds(Optional(FieldsAre(absl::StrCat("[2/2] ", file2),
                                              BufferIs("Test data")))));
  EXPECT_THAT(provider.GetNext(), IsOkAndHolds(Eq(std::nullopt)));
}

TEST(FilePerfDataProvider, PropagatesErrors) {
  FilePerfDataProvider provider(
      {absl::StrCat(FLAGS_test_tmpdir,
                    "/FilePerfDataProvider_PropagatesErrors_does_not_exist")});
  EXPECT_THAT(provider.GetNext(),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("Error in llvm::MemoryBuffer::getFile")));
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
