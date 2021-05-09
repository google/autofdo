// Checks that llvm profile reader reads in profiles correctly.

#include "llvm_profile_reader.h"

#include "base/commandlineflags.h"
#include "symbol_map.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/container/node_hash_set.h"
#include "third_party/abseil/absl/flags/flag.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

void VerifySymbolMap(const devtools_crosstool_autofdo::SymbolMap &symbol_map) {
  const auto *data = symbol_map.map().at("_Z11compute_noii");
  constexpr uint16_t kLine = 3;
  constexpr uint16_t kDisc = 2;
  constexpr uint32_t kLookup = kLine << 16 | kDisc;

  EXPECT_EQ(data->callsites.size(), 0);
  EXPECT_EQ(data->pos_counts.size(), 16);
  EXPECT_EQ(data->pos_counts.at(kLookup).count, 1941);
  data = symbol_map.map().at("main");

  EXPECT_EQ(data->pos_counts.size(), 16);
  EXPECT_EQ(data->callsites.size(), 1);
}

TEST(LLVMProfileReaderTest, ReadBinaryTest) {
  devtools_crosstool_autofdo::SymbolMap symbol_map;
  absl::node_hash_set<std::string> names;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map, names);
  reader.ReadFromFile(FLAGS_test_srcdir +
                      "/testdata/"
                      "llvm_autoprof.golden.binprof");
  VerifySymbolMap(symbol_map);
}

TEST(LLVMProfileReaderTest, ReadTextTest) {
  devtools_crosstool_autofdo::SymbolMap symbol_map;
  absl::node_hash_set<std::string> names;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map, names);
  EXPECT_TRUE(
      reader.ReadFromFile(FLAGS_test_srcdir +
                          "/testdata/"
                          "llvm_autoprof.golden.textprof"));

  VerifySymbolMap(symbol_map);
}

TEST(LLVMProfileReaderTest, ReadEmptyBodyNonZeroFunctionTotalTest) {
  devtools_crosstool_autofdo::SymbolMap symbol_map;
  absl::node_hash_set<std::string> names;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map, names);
  reader.ReadFromFile(FLAGS_test_srcdir +
                      "/testdata/"
                      "llvm_testzero.golden.textprof");
  const auto *data =
      symbol_map.map().at("_Z11EmptyBodyNonZeroTotalCount_noii");

  EXPECT_EQ(data->total_count, 1000);
}
}  // namespace
