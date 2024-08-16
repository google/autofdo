#include "addr2line.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/testing/status_matchers.h"

namespace {

using ::devtools_crosstool_autofdo::Addr2line;

TEST(Addr2lineTest, Dwarf2Dwarf5Binary) {
  const std::string binary =
      absl::StrCat(::testing::SrcDir(),
                   "/testdata/"
                   "dwarf2_dwarf5.bin");

  Addr2line* addr2line = Addr2line::Create(binary);
  EXPECT_TRUE(addr2line != NULL);
}
}  // namespace
