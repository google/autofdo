// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// These tests check to see that symbol_map can read correct information
// from the binary.
#include "symbol_map.h"

#include <cstdint>

#include "base/logging.h"
#include "llvm_profile_reader.h"
#include "source_info.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/container/node_hash_set.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/types/optional.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

using ::devtools_crosstool_autofdo::SymbolMap;
using ::devtools_crosstool_autofdo::SourceStack;

class SymbolMapTest : public testing::Test {
 protected:
  SymbolMapTest() {}
};

const char kTestDataDir[] =
    "/testdata/";

TEST(SymbolMapTest, SymbolMap) {
  SymbolMap symbol_map(
      FLAGS_test_srcdir + kTestDataDir + "test.binary");

  // Check if the symbol table is correctly built.
  EXPECT_EQ(symbol_map.size(), 0);
  EXPECT_TRUE(symbol_map.map().find("not_exist") == symbol_map.map().end());

  symbol_map.AddSymbol("foo");

  // Check if AddSourceCount works correctly.
  SourceStack stack;
  devtools_crosstool_autofdo::SourceInfo tuple1("bar", "", "", 0, 25, 0);
  devtools_crosstool_autofdo::SourceInfo tuple2("foo", "", "", 0, 50, 0);
  stack.push_back(tuple1);
  stack.push_back(tuple2);
  symbol_map.AddSourceCount("foo", stack, 100, 2);
  // Add "foo" only source count.
  devtools_crosstool_autofdo::SourceInfo tuple3("foo", "", "", 0, 49, 0);
  stack.clear();
  stack.push_back(tuple3);
  symbol_map.AddSourceCount("foo", stack, 150, 2);
  const devtools_crosstool_autofdo::Symbol *symbol =
      symbol_map.map().find("foo")->second;
  ASSERT_TRUE(symbol != NULL);
  EXPECT_EQ(symbol->pos_counts.size(), 1);
  EXPECT_EQ(symbol->callsites.size(), 1);
  EXPECT_EQ(symbol->EntryCount(), 150);
  const devtools_crosstool_autofdo::Symbol *bar =
      symbol->callsites.find(std::make_pair(tuple2.Offset(false), "bar"))
          ->second;
  ASSERT_TRUE(bar != NULL);
  EXPECT_EQ(bar->total_count, 100);
  EXPECT_EQ(bar->callsites.size(), 0);
  EXPECT_EQ(bar->pos_counts.size(), 1);
  EXPECT_EQ(bar->EntryCount(), 100);
  const auto &ret = bar->pos_counts.find(tuple1.Offset(false));
  ASSERT_TRUE(ret != bar->pos_counts.end());
  EXPECT_EQ(ret->second.count, 100);
  EXPECT_EQ(ret->second.num_inst, 2);
  EXPECT_FALSE(symbol_map.Validate());
}

TEST(SymbolMapTest, TestEntryCount) {
  SymbolMap symbol_map(
      FLAGS_test_srcdir + kTestDataDir + "test.binary");

  symbol_map.AddSymbol("foo");

  // foo->bar->baz->qux ==> callsites
  SourceStack stack1 = {
      {"qux", "", "", 0, 10, 0},
      {"baz", "", "", 0, 20, 0},
      {"bar", "", "", 0, 25, 0},
      {"foo", "", "", 0, 50, 0},
  };
  symbol_map.AddSourceCount("foo", stack1, 100, 2);

  // foo->bar->baz ==> callsites
  SourceStack stack2 = {
      {"baz", "", "", 0, 30, 0},
      {"bar", "", "", 0, 25, 0},
      {"foo", "", "", 0, 50, 0},
  };
  symbol_map.AddSourceCount("foo", stack2, 0, 2);

  // foo only ==> pos_counts
  SourceStack stack3 = {
      {"foo", "", "", 0, 55, 0},
  };
  symbol_map.AddSourceCount("foo", stack3, 150, 2);

  const devtools_crosstool_autofdo::Symbol *symbol =
      symbol_map.map().find("foo")->second;
  EXPECT_EQ(symbol->EntryCount(), 100);

  const devtools_crosstool_autofdo::Symbol *bar =
      symbol->callsites.find(std::make_pair(stack2[2].Offset(false), "bar"))
          ->second;
  EXPECT_EQ(bar->EntryCount(), 100);

  const devtools_crosstool_autofdo::Symbol *baz =
      bar->callsites.find(std::make_pair(stack2[1].Offset(false), "baz"))
          ->second;
  EXPECT_EQ(baz->EntryCount(), 100);

  const devtools_crosstool_autofdo::Symbol *qux =
      baz->callsites.find(std::make_pair(stack1[1].Offset(false), "qux"))
          ->second;
  EXPECT_EQ(qux->EntryCount(), 100);
}

TEST(SymbolMapTest, ComputeAllCounts) {
  SymbolMap symbol_map;
  absl::node_hash_set<std::string> names;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map, names);
  reader.ReadFromFile(FLAGS_test_srcdir + kTestDataDir +
                      "callgraph_with_cycles.txt");

  // Check ComputeAllCounts will set up correct allcounts for all
  // the symbols.
  symbol_map.ComputeTotalCountIncl();
  const devtools_crosstool_autofdo::NameSymbolMap &map = symbol_map.map();
  EXPECT_EQ(map.find("main")->second->total_count_incl, 21414);
  EXPECT_EQ(map.find("goo")->second->total_count_incl, 15050);
  EXPECT_EQ(map.find("hoo")->second->total_count_incl, 15050);
  EXPECT_EQ(map.find("moo")->second->total_count_incl, 50);
}

std::string GenRandomName(const int len) {
  std::string result(len, '\0');
  static const char alpha[] = "abcdefghijklmnopqrstuvwxyz";
  for (char &c : result) c = alpha[rand() % (sizeof(alpha) - 1)];
  return result;
}

TEST(SymbolMapTest, TestElideSuffixesAndMerge) {
  unsigned seed = static_cast<unsigned>(time(nullptr)) * getpid();
  LOG(INFO) << "Seed of the srand function: " << seed;
  srand(seed);
  SymbolMap symbol_map;
  for (int i = 0; i < 20; i++) {
    std::string name = GenRandomName(10);
    symbol_map.AddSymbol(name);
    symbol_map.AddSymbolEntryCount(name, 100, 1000);

    name = GenRandomName(10) + ".cold";
    symbol_map.AddSymbol(name);
    symbol_map.AddSymbolEntryCount(name, 100, 1000);
  }

  symbol_map.set_suffix_elision_policy("all");
  symbol_map.ElideSuffixesAndMerge();

  for (const auto &sym : symbol_map.map()) {
    if (symbol_map.ShouldEmit(sym.second->total_count))
      EXPECT_PRED_FORMAT2(testing::IsNotSubstring, ".cold", sym.first);
  }
}

TEST(SymbolMapTest, TestInterestingSymbolNames) {
  const char *policies[] = { "all", "none", "selected" };
  for (auto p : policies) {
    std::string policy(p);
    SymbolMap symbol_map;
    symbol_map.set_suffix_elision_policy(policy);
    absl::node_hash_set<std::string> names;
    devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map, names);
        reader.ReadFromFile(FLAGS_test_srcdir + kTestDataDir +
                            "symbols_with_fun_characters.txt");

        const devtools_crosstool_autofdo::NameSymbolMap &map = symbol_map.map();
        auto main_iter = map.find("main");
        ASSERT_NE(main_iter, map.end());

        const devtools_crosstool_autofdo::PositionCountMap &pos_counts =
            main_iter->second->pos_counts;

        ASSERT_EQ(pos_counts.size(), 1);
        const devtools_crosstool_autofdo::CallTargetCountMap &target_counts =
            pos_counts.begin()->second.target_map;

        auto get_count =
            [&](std::string target_name) -> absl::optional<uint64> {
          auto iter = target_counts.find(target_name);
          if (iter == target_counts.end()) return absl::nullopt;
          return iter->second;
        };

        std::cerr << "policy=" << policy << "\n";
        if (policy == "all") {
          EXPECT_EQ(get_count("..foo.bar"), 800);
          EXPECT_EQ(get_count("foo"), 100);
          EXPECT_EQ(get_count("glom"), 50);
          EXPECT_EQ(get_count("a"), 50);
          EXPECT_EQ(get_count("goo"), 70);
          EXPECT_EQ(get_count("xoo"), 120);
        } else if (policy == "selected") {
          EXPECT_EQ(get_count("..foo.bar"), 800);
          EXPECT_EQ(get_count("foo"), 100);
          EXPECT_EQ(get_count("glom.something"), 50);
          EXPECT_EQ(get_count("a.b.part1"), 50);
          EXPECT_EQ(get_count("goo"), 70);
          EXPECT_EQ(get_count("xoo.part2"), 120);
        } else if (policy == "none") {
          EXPECT_EQ(get_count("..foo.bar"), 800);
          EXPECT_EQ(get_count("foo.llvm.1234"), 100);
          EXPECT_EQ(get_count("glom.something"), 50);
          EXPECT_EQ(get_count("a.b.llvm.567.part1"), 50);
          EXPECT_EQ(get_count("goo.llvm.2835.cold"), 70);
          EXPECT_EQ(get_count("xoo.cold.part2"), 120);
        }
  }
}

TEST(SymbolMapTest, IsLLVMCompiler) {
  EXPECT_TRUE(SymbolMap::IsLLVMCompiler(
      "gcc-4.X.Y-crosstool-v18-llvm-grtev4-k8-fastbuild"));
  EXPECT_FALSE(SymbolMap::IsLLVMCompiler(
      "gcc-4.X.Y-crosstool-v18-hybrid-grtev4-k8-fastbuild"));
}


TEST(SymbolMapTest, BuildFlatProfile) {
  SymbolMap sm;
  sm.AddSymbol("foo");
  // foo->bar->baz->qux ==> callsites
  SourceStack stack = {
      {"qux", "", "", 0, 10, 0},
      {"baz", "", "", 0, 20, 0},
      {"bar", "", "", 0, 25, 0},
      {"foo", "", "", 0, 50, 0},
  };
  sm.AddSourceCount("foo", stack, 100, 2);
  SymbolMap got;
  uint64_t total = 0, numFlattened = 0;
  got.BuildFlatProfile(sm, false, 0, total, numFlattened);

  // Expect that inline_call is flattened
  EXPECT_TRUE(got.map().find("bar") != got.map().end());
  EXPECT_TRUE(got.map().find("baz") != got.map().end());
  EXPECT_TRUE(got.map().find("qux") != got.map().end());
  EXPECT_EQ(total, 1);
  EXPECT_EQ(numFlattened, 1);
}

// foo -> 20000
//   bar -> 20000
//     baz -> 10000
//       qux -> 10000
// cold_fn -> 99
//   cold_callsite1 -> 99
//     cold_callsite2 -> 20
//       cold_callsite3 -> 20
void InitializeSymbolMap(SymbolMap &sm) {
  sm.AddSymbol("foo");
  // foo->bar->baz->qux ==> callsites
  SourceStack stack1 = {
      {"qux", "", "", 0, 10, 0},
      {"baz", "", "", 0, 20, 0},
      {"bar", "", "", 0, 25, 0},
      {"foo", "", "", 0, 50, 0},
  };
  sm.AddSourceCount("foo", stack1, 10000, 2);
  sm.AddSourceCount("foo", {stack1[2], stack1[3]}, 10000, 2);

  sm.AddSymbol("cold_fn");
  // cold_fn->cold_callsite1->cold_callsite2->cold_callsite3 ==> callsites
  SourceStack stack2 = {
      {"cold_callsite3", "", "", 0, 10, 0},
      {"cold_callsite2", "", "", 0, 20, 0},
      {"cold_callsite1", "", "", 0, 25, 0},
      {"cold_fn", "", "", 0, 50, 0},
  };
  sm.AddSourceCount("cold_fn", stack2, 20, 2);
  sm.AddSourceCount("cold_fn", {stack2[2], stack2[3]}, 79, 2);
}

TEST(SymbolMapTest, BuildSelectivelyFlatProfile) {
  SymbolMap sm;
  InitializeSymbolMap(sm);
  SymbolMap got;
  uint64_t total = 0, numFlattened = 0;
  got.BuildFlatProfile(sm, true, 100, total, numFlattened);

  // Expect that hot function (foo) is not flattened
  EXPECT_FALSE(got.map().find("bar") != got.map().end());
  EXPECT_FALSE(got.map().find("baz") != got.map().end());
  EXPECT_FALSE(got.map().find("qux") != got.map().end());
  // Expect that cold fn is flattened i.e. the inlined callsite should be
  // converted to an indirect call. Offset << 16 is how its stored in the source
  // location
  EXPECT_FALSE(got.map().find("cold_callsite1") != got.map().end());
  EXPECT_TRUE(got.map().at("cold_fn")->pos_counts[50 << 16].target_map.find(
                  "cold_callsite1") !=
              got.map().at("cold_fn")->pos_counts[50 << 16].target_map.end());
  EXPECT_EQ(total, 2);
  EXPECT_EQ(numFlattened, 1);
}

TEST(SymbolMapTest, BuildHybridProfile) {
  SymbolMap sm;
  InitializeSymbolMap(sm);
  SymbolMap got;
  uint64_t num_callsites = 0, num_callsites_flattened = 0;

  // Smaller threshold value leads to lesser flattening
  got.BuildHybridProfile(sm, 21, num_callsites, num_callsites_flattened);

  // Callsites above the threshold should not be flattened.
  EXPECT_FALSE(got.map().find("bar") != got.map().end());
  EXPECT_FALSE(got.map().find("baz") != got.map().end());
  EXPECT_FALSE(got.map().find("qux") != got.map().end());
  EXPECT_FALSE(got.map().find("cold_callsite1") != got.map().end());
  // Callsites below the threshold should be flattened.
  EXPECT_TRUE(got.map().find("cold_callsite2") != got.map().end());
  EXPECT_TRUE(got.map().find("cold_callsite3") != got.map().end());
  EXPECT_EQ(num_callsites, 6);
  EXPECT_EQ(num_callsites_flattened, 2);


  num_callsites = 0;
  num_callsites_flattened = 0;
  // Higher threshold flattens most callsites even in hot functions
  got.BuildHybridProfile(sm, 10001, num_callsites, num_callsites_flattened);

  // Callsites above the threshold should not be flattened.
  EXPECT_FALSE(got.map().find("bar") != got.map().end());
  // All other callsites should be flattened and converted to direct calls.
  EXPECT_TRUE(got.map().find("baz") != got.map().end());
  EXPECT_TRUE(got.map().find("qux") != got.map().end());
  EXPECT_TRUE(got.map().find("cold_callsite1") != got.map().end());
  ASSERT_TRUE(got.map().find("cold_fn") != got.map().end());
  EXPECT_TRUE(got.map().at("cold_fn")->pos_counts[50 << 16].target_map.find(
                  "cold_callsite1") !=
              got.map().at("cold_fn")->pos_counts[50 << 16].target_map.end());
  EXPECT_TRUE(got.map().find("cold_callsite2") != got.map().end());
  EXPECT_TRUE(got.map().find("cold_callsite3") != got.map().end());
  EXPECT_EQ(num_callsites, 6);
  EXPECT_EQ(num_callsites_flattened, 5);
}

}  // namespace
