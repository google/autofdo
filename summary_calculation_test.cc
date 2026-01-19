#include <array>
#include <string>

#include "profile_writer.h"
#include "symbol_map.h"
#include "gtest/gtest.h"

using namespace devtools_crosstool_autofdo;

TEST(SummaryCalculationTest, SummaryCalculator) {
  std::string binary = ::testing::SrcDir() + "/testdata/test.binary";

  // Taken from SymbolMapTest::TestEntryCount
  SymbolMap symbol_map(binary);
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

  ProfileSummaryInformation info = ProfileSummaryComputer::Compute(
      symbol_map, {std::begin(ProfileSummaryInformation::default_cutoffs),
                   std::end(ProfileSummaryInformation::default_cutoffs)});

  EXPECT_EQ(info.total_count_, 1000);
  EXPECT_EQ(info.max_count_, 450);
  EXPECT_EQ(info.max_function_count_, 300);
  EXPECT_EQ(info.num_counts_, 6);
  EXPECT_EQ(info.num_functions_, 2);
  EXPECT_EQ(info.detailed_summaries_.size(), 16);

  std::array<std::tuple<int, int, int>, 16> expected_percentiles = {
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

  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(info.detailed_summaries_[i].cutoff_, std::get<0>(expected_percentiles[i]));
    EXPECT_EQ(info.detailed_summaries_[i].min_count_, std::get<1>(expected_percentiles[i]));
    EXPECT_EQ(info.detailed_summaries_[i].num_counts_, std::get<2>(expected_percentiles[i]));
  }
}
