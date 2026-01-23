#include <array>
#include <string>

#include "profile_reader.h"
#include "profile_writer.h"
#include "symbol_map.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace devtools_crosstool_autofdo;

namespace {
using ::testing::Eq;

void InitializeSymbolMap(SymbolMap &symbol_map) {
  // Taken from SymbolMapTest::TestEntryCount
  symbol_map.AddSymbol("foo");
  symbol_map.AddSymbolEntryCount("foo", 200);

  // foo->bar->baz->qux ==> callsites
  SourceStack foo_stack1 = {
      {"qux", "", "", 0, 10, 0},
      {"baz", "", "", 0, 20, 0},
      {"bar", "", "", 0, 25, 0},
      {"foo", "", "", 0, 50, 0},
  };
  symbol_map.AddSourceCount("foo", foo_stack1, 300, 2);

  // foo->bar->baz ==> callsites
  SourceStack foo_stack2 = {
      {"baz", "", "", 0, 30, 0},
      {"bar", "", "", 0, 25, 0},
      {"foo", "", "", 0, 50, 0},
  };
  symbol_map.AddSourceCount("foo", foo_stack2, 0, 2);

  // foo only ==> pos_counts
  SourceStack foo_stack3 = {
      {"foo", "", "", 0, 55, 0},
  };
  symbol_map.AddSourceCount("foo", foo_stack3, 450, 2);

  symbol_map.AddSymbol("boo");
  symbol_map.AddSymbolEntryCount("boo", 300);

  // boo->dar->daz->dux ==> callsites
  SourceStack boo_stack1 = {
      {"dux", "", "", 0, 10, 0},
      {"daz", "", "", 0, 20, 0},
      {"dar", "", "", 0, 25, 0},
      {"boo", "", "", 0, 50, 0},
  };
  symbol_map.AddSourceCount("boo", boo_stack1, 100, 2);

  // boo->dar->daz ==> callsites
  SourceStack boo_stack2 = {
      {"daz", "", "", 0, 30, 0},
      {"dar", "", "", 0, 25, 0},
      {"boo", "", "", 0, 50, 0},
  };
  symbol_map.AddSourceCount("boo", boo_stack2, 0, 2);

  // boo only ==> pos_counts
  SourceStack boo_stack3 = {
      {"boo", "", "", 0, 55, 0},
  };
  symbol_map.AddSourceCount("boo", boo_stack3, 150, 2);
}

// clang-format off
std::array<std::tuple<int, int, int>, 16> ExpectedPercentiles = {
  std::tuple<int, int, int>{10000, 450, 1},
  {100000, 450, 1},
  {200000, 450, 1},
  {300000, 450, 1},
  {400000, 450, 1},
  {500000, 300, 2},
  {600000, 300, 2},
  {700000, 300, 2},
  {800000, 150, 3},
  {900000, 150, 3},
  {950000, 100, 4},
  {990000, 100, 4},
  {999000, 100, 4},
  {999900, 100, 4},
  {999990, 100, 4},
  {999999, 100, 4},
};
// clang-format on

void VerifySummaryInformation(ProfileSummaryInformation &info) {
  EXPECT_EQ(info.total_count_, 1000);
  EXPECT_EQ(info.max_count_, 450);
  EXPECT_EQ(info.max_function_count_, 300);
  EXPECT_EQ(info.num_counts_, 6);
  EXPECT_EQ(info.num_functions_, 2);
  EXPECT_EQ(info.detailed_summaries_.size(), 16);

  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(info.detailed_summaries_[i].cutoff_,
              std::get<0>(ExpectedPercentiles[i]));
    EXPECT_EQ(info.detailed_summaries_[i].min_count_,
              std::get<1>(ExpectedPercentiles[i]));
    EXPECT_EQ(info.detailed_summaries_[i].num_counts_,
              std::get<2>(ExpectedPercentiles[i]));
  }
}

TEST(ProfileSummaryCalculator, SummaryCalculationTest) {
  std::string binary = ::testing::SrcDir() + "/testdata/test.binary";

  SymbolMap symbol_map(binary);
  InitializeSymbolMap(symbol_map);

  ProfileSummaryInformation info = ProfileSummaryComputer::Compute(
      symbol_map, {std::begin(ProfileSummaryInformation::default_cutoffs),
                   std::end(ProfileSummaryInformation::default_cutoffs)});

  // Verify that the summary was calculated correctly.
  VerifySummaryInformation(info);
}

TEST(ProfileSummaryCalculator, SummaryReadWriteTest) {
  std::string binary = ::testing::SrcDir() + "/testdata/test.binary";

  SymbolMap symbol_map_1(binary);
  InitializeSymbolMap(symbol_map_1);

  char template_name[] = "summary_read_test.XXXXXX";
  const char *name = mktemp(template_name);
  ASSERT_NE(name, nullptr);

  // Write out the summary information.
  AutoFDOProfileWriter writer(&symbol_map_1, 3);
  writer.WriteToFile(name);

  // Read the summary information back in.
  SymbolMap symbol_map_2;
  AutoFDOProfileReader reader(&symbol_map_2, false);
  reader.ReadFromFile(name);

  // Re-calculate the summary that was written out.
  ProfileSummaryInformation info_1 = ProfileSummaryComputer::Compute(
      symbol_map_1, {std::begin(ProfileSummaryInformation::default_cutoffs),
                     std::end(ProfileSummaryInformation::default_cutoffs)});
  // Verify that this was calculated correctly.
  VerifySummaryInformation(info_1);

  // Get the summary that was read back in.
  ProfileSummaryInformation *info_2 = reader.GetSummaryInformation();
  ASSERT_NE(info_2, nullptr);

  // Make sure the summary that was written out is the same as that which is
  // read back in.
  EXPECT_THAT(info_1, Eq(*info_2));
  remove(name);
}

} // namespace
