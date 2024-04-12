#include "addr2cu.h"

#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <utility>

#include "base/logging.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "util/testing/status_matchers.h"

namespace {

using ::devtools_crosstool_autofdo::Addr2Cu;
using ::devtools_crosstool_autofdo::CreateDWARFContext;
using ::testing::HasSubstr;
using ::testing::status::IsOkAndHolds;
using ::testing::status::StatusIs;

uint64_t GetSymbolAddress(const std::string &symmap, absl::string_view symbol) {
  std::ifstream fin(symmap.c_str());
  int64_t addr;
  std::string sym_type;
  std::string sym_name;
  while (fin >> std::dec >> addr >> sym_type >> sym_name)
    if (sym_name == symbol) return addr;
  return -1;
}

struct BinaryData {
  std::unique_ptr<llvm::MemoryBuffer> mem_buf;
  std::unique_ptr<llvm::object::ObjectFile> object_file;
};

// Primes `BinaryData` for test cases.
absl::StatusOr<BinaryData> SetupBinaryData(absl::string_view binary) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> mem_buf =
      llvm::MemoryBuffer::getFile(binary);
  if (!mem_buf) {
    return absl::FailedPreconditionError(absl::StrCat(
        "failed creating MemoryBuffer: %s", mem_buf.getError().message()));
  }

  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> object_file =
      llvm::object::ObjectFile::createELFObjectFile(**mem_buf);
  if (!object_file) {
    return absl::FailedPreconditionError(
        absl::StrFormat("failed creating ELFObjectFile: %s",
                        llvm::toString(object_file.takeError())));
  }

  return BinaryData{.mem_buf = std::move(*mem_buf),
                    .object_file = std::move(*object_file)};
}

TEST(Addr2CuTest, ComdatFuncHasNoDwp) {
  const std::string binary =
      absl::StrCat(::testing::SrcDir(),
                   "/testdata/"
                   "test_comdat_with_dwp.bin");

  ASSERT_OK_AND_ASSIGN(BinaryData binary_data, SetupBinaryData(binary));

  EXPECT_THAT(CreateDWARFContext(*binary_data.object_file),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("without a corresponding dwp file")));
}
}  // namespace
