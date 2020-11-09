// Checks that llvm profile reader reads in profiles correctly.

#include "llvm_profile_reader.h"

#include "base/commandlineflags.h"
#include "symbol_map.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

class LLVMProfileReaderTest : public testing::Test {};


void VerifySymbolMap(const devtools_crosstool_autofdo::SymbolMap &symbol_map) {
  const auto *data = symbol_map.map().find("_Z11compute_noii")->second;
  constexpr uint16_t kLine = 3;
  constexpr uint16_t kDisc = 2;
  constexpr uint32_t kLookup = kLine << 16 | kDisc;

  EXPECT_EQ(data->callsites.size(), 0);
  EXPECT_EQ(data->pos_counts.size(), 16);
  EXPECT_EQ(data->pos_counts.find(kLookup)->second.count, 1941);
  data = symbol_map.map().find("main")->second;

  EXPECT_EQ(data->pos_counts.size(), 16);
  EXPECT_EQ(data->callsites.size(), 1);
}

TEST_F(LLVMProfileReaderTest, ReadBinaryTest) {
  devtools_crosstool_autofdo::SymbolMap symbol_map;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map);
  reader.ReadFromFile(FLAGS_test_srcdir +
                      "/testdata/"
                      "llvm_autoprof.golden.binprof");
  VerifySymbolMap(symbol_map);
}

TEST_F(LLVMProfileReaderTest, ReadTextTest) {
  devtools_crosstool_autofdo::SymbolMap symbol_map;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map);
  reader.ReadFromFile(FLAGS_test_srcdir +
                      "/testdata/"
                      "llvm_autoprof.golden.textprof");

  VerifySymbolMap(symbol_map);
}
}  // namespace
