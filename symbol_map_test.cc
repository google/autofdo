// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// These tests check to see that symbol_map can read correct information
// from the binary.
#include "symbol_map.h"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include "base/logging.h"
#include "llvm_profile_reader.h"
#include "source_info.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/container/node_hash_set.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/str_cat.h"

namespace {

using ::devtools_crosstool_autofdo::Callsite;
using ::devtools_crosstool_autofdo::SymbolMap;
using ::devtools_crosstool_autofdo::SourceStack;

class SymbolMapTest : public testing::Test {
 protected:
  SymbolMapTest() {}
};

const char kTestDataDir[] =
    "/testdata/";

TEST(SymbolMapTest, SymbolMap) {
  SymbolMap symbol_map(::testing::SrcDir() + kTestDataDir + "test.binary");

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
  ASSERT_TRUE(symbol != nullptr);
  EXPECT_EQ(symbol->pos_counts.size(), 1);
  EXPECT_EQ(symbol->callsites.size(), 1);
  EXPECT_EQ(symbol->EntryCount(), 150);
  const devtools_crosstool_autofdo::Symbol *bar =
      symbol->callsites.find(Callsite{tuple2.Offset(false), "bar"})
          ->second;
  ASSERT_TRUE(bar != nullptr);
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
  SymbolMap symbol_map(::testing::SrcDir() + kTestDataDir + "test.binary");

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
      symbol->callsites.find(Callsite{stack2[2].Offset(false), "bar"})
          ->second;
  EXPECT_EQ(bar->EntryCount(), 100);

  const devtools_crosstool_autofdo::Symbol *baz =
      bar->callsites.find(Callsite{stack2[1].Offset(false), "baz"})
          ->second;
  EXPECT_EQ(baz->EntryCount(), 100);

  const devtools_crosstool_autofdo::Symbol *qux =
      baz->callsites.find(Callsite{stack1[1].Offset(false), "qux"})
          ->second;
  EXPECT_EQ(qux->EntryCount(), 100);
}

TEST(SymbolMapTest, ComputeAllCounts) {
  SymbolMap symbol_map;
  absl::node_hash_set<std::string> names;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map, names);
  reader.ReadFromFile(::testing::SrcDir() + kTestDataDir +
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
    reader.ReadFromFile(::testing::SrcDir() + kTestDataDir +
                        "symbols_with_fun_characters.txt");

    const devtools_crosstool_autofdo::NameSymbolMap &map = symbol_map.map();
    auto main_iter = map.find("main");
    ASSERT_NE(main_iter, map.end());

    const devtools_crosstool_autofdo::PositionCountMap &pos_counts =
        main_iter->second->pos_counts;

    ASSERT_EQ(pos_counts.size(), 1);
    const devtools_crosstool_autofdo::CallTargetCountMap &target_counts =
        pos_counts.begin()->second.target_map;

    auto get_count = [&](std::string target_name) -> std::optional<uint64_t> {
      auto iter = target_counts.find(target_name);
      if (iter == target_counts.end()) return std::nullopt;
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
  // converted to an indirect call. Offset << 32 is how its stored in the source
  // location
  EXPECT_FALSE(got.map().find("cold_callsite1") != got.map().end());
  EXPECT_TRUE(got.map().at("cold_fn")->pos_counts[50l << 32].target_map.find(
                  "cold_callsite1") !=
              got.map().at("cold_fn")->pos_counts[50l << 32].target_map.end());
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
  EXPECT_TRUE(got.map().at("cold_fn")->pos_counts[50l << 32].target_map.find(
                  "cold_callsite1") !=
              got.map().at("cold_fn")->pos_counts[50l << 32].target_map.end());
  EXPECT_TRUE(got.map().find("cold_callsite2") != got.map().end());
  EXPECT_TRUE(got.map().find("cold_callsite3") != got.map().end());
  EXPECT_EQ(num_callsites, 6);
  EXPECT_EQ(num_callsites_flattened, 5);
}

TEST(SymbolMapTest, FSDiscriminator) {
  absl::SetFlag(&FLAGS_use_fs_discriminator, false);
  SymbolMap symbol_map1(::testing::SrcDir() + kTestDataDir + "test.binary");
  // Check if the use_fs_discriminaor is correctly set to false.
  EXPECT_FALSE(devtools_crosstool_autofdo::SourceInfo::use_fs_discriminator);
  // Check if the use_base_only_in_fs_discriminator is correctly set.
  EXPECT_FALSE(devtools_crosstool_autofdo::SourceInfo::
                   use_base_only_in_fs_discriminator);

  absl::SetFlag(&FLAGS_use_fs_discriminator, false);
  SymbolMap symbol_map2(::testing::SrcDir() + kTestDataDir + "test.fs.binary");
  // Check if the use_fs_discriminaor is correctly set.
  EXPECT_TRUE(devtools_crosstool_autofdo::SourceInfo::use_fs_discriminator);
  // Check if the use_base_only_in_fs_discriminaor is correctly set.
  EXPECT_FALSE(devtools_crosstool_autofdo::SourceInfo::
                   use_base_only_in_fs_discriminator);
}

TEST(SymbolMapTest, FSDiscriminatorUseBaseOnly) {
  absl::SetFlag(&FLAGS_use_fs_discriminator, false);
  absl::SetFlag(&FLAGS_use_base_only_in_fs_discriminator, true);
  SymbolMap symbol_map1(::testing::SrcDir() + kTestDataDir + "test.binary");
  // Check if the use_fs_discriminaor is correctly set to false.
  EXPECT_FALSE(devtools_crosstool_autofdo::SourceInfo::use_fs_discriminator);
  // Check if the use_base_only_in_fs_discriminaor is correctly set.
  EXPECT_TRUE(devtools_crosstool_autofdo::SourceInfo::
                  use_base_only_in_fs_discriminator);

  absl::SetFlag(&FLAGS_use_fs_discriminator, false);
  absl::SetFlag(&FLAGS_use_base_only_in_fs_discriminator, true);
  SymbolMap symbol_map2(::testing::SrcDir() + kTestDataDir + "test.fs.binary");
  // Check if the use_fs_discriminaor is correctly set.
  EXPECT_TRUE(devtools_crosstool_autofdo::SourceInfo::use_fs_discriminator);
  // Check if the use_base_only_in_fs_discriminaor is correctly set.
  EXPECT_TRUE(devtools_crosstool_autofdo::SourceInfo::
                  use_base_only_in_fs_discriminator);
}
TEST(SymbolMapTest, RemoveSymsMatchingRegex) {
  SymbolMap symbol_map;
  absl::node_hash_set<std::string> names;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map, names);
  reader.ReadFromFile(::testing::SrcDir() + kTestDataDir +
                      "strip_symbols_regex.textprof");

  const devtools_crosstool_autofdo::NameSymbolMap &map = symbol_map.map();

  EXPECT_EQ(map.find("_ZNK4llvm12SCEVUDivExpr7getTypeEv")->second->total_count,
            7200);
  EXPECT_EQ(map.find("llvmSCEV")->second->total_count, 4500);
  EXPECT_EQ(map.find("getType")->second->total_count, 50);
  EXPECT_EQ(map.find("_ZNK4llvm4SCEV7getTypeEv")->second->total_count, 620);
  EXPECT_EQ(map.find("_ZNK4llvm11SCEVMulExpr7getTypeEv")->second->total_count,
            592);

  symbol_map.RemoveSymsMatchingRegex(".*SCEV.*getType.*");

  // Check the symbols matching regular expression ".*SCEV.*getType.*" has
  // been set to symbols with zero counts. They will be removed in when
  // profile is written out.
  EXPECT_EQ(map.find("_ZNK4llvm12SCEVUDivExpr7getTypeEv")->second->total_count,
            0);
  EXPECT_EQ(map.find("llvmSCEV")->second->total_count, 4500);
  EXPECT_EQ(map.find("getType")->second->total_count, 50);
  EXPECT_EQ(map.find("_ZNK4llvm4SCEV7getTypeEv")->second->total_count, 0);
  EXPECT_EQ(map.find("_ZNK4llvm11SCEVMulExpr7getTypeEv")->second->total_count,
            0);
}

TEST(SymbolMapTest, throttleInlineInstancesAtSameLocation) {
  SymbolMap symbol_map;
  absl::node_hash_set<std::string> names;
  devtools_crosstool_autofdo::LLVMProfileReader reader(&symbol_map, names);
  reader.ReadFromFile(::testing::SrcDir() + kTestDataDir +
                      "throttle_inline_instances.textprof");

  const devtools_crosstool_autofdo::NameSymbolMap &map = symbol_map.map();

  devtools_crosstool_autofdo::Symbol *foo_symbol = map.find("foo")->second;
  devtools_crosstool_autofdo::CallsiteMap &foo_cs_map = foo_symbol->callsites;
  EXPECT_EQ(foo_cs_map.size(), 5);

  devtools_crosstool_autofdo::SourceInfo tuple1("foo", "", "", 0, 2, 1);
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple1.Offset(false), "goo1"}) !=
              foo_cs_map.end());
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple1.Offset(false), "goo2"}) !=
              foo_cs_map.end());
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple1.Offset(false), "goo3"}) !=
              foo_cs_map.end());
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple1.Offset(false), "goo4"}) !=
              foo_cs_map.end());
  devtools_crosstool_autofdo::SourceInfo tuple2("foo", "", "", 0, 2, 4);
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple2.Offset(false), "hoo"}) !=
              foo_cs_map.end());
  devtools_crosstool_autofdo::Symbol *hoo_symbol =
      foo_cs_map.find(Callsite{tuple2.Offset(false), "hoo"})->second;
  devtools_crosstool_autofdo::CallsiteMap &hoo_cs_map = hoo_symbol->callsites;
  EXPECT_EQ(hoo_cs_map.size(), 4);

  devtools_crosstool_autofdo::SourceInfo tuple3("hoo", "", "", 0, 3, 2);
  EXPECT_TRUE(hoo_cs_map.find(Callsite{tuple3.Offset(false), "bar1"}) !=
              hoo_cs_map.end());
  EXPECT_TRUE(hoo_cs_map.find(Callsite{tuple3.Offset(false), "bar2"}) !=
              hoo_cs_map.end());
  EXPECT_TRUE(hoo_cs_map.find(Callsite{tuple3.Offset(false), "bar3"}) !=
              hoo_cs_map.end());
  EXPECT_TRUE(hoo_cs_map.find(Callsite{tuple3.Offset(false), "bar4"}) !=
              hoo_cs_map.end());

  symbol_map.throttleInlineInstancesAtSameLocation(2);

  EXPECT_EQ(foo_cs_map.size(), 3);
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple1.Offset(false), "goo1"}) ==
              foo_cs_map.end());
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple1.Offset(false), "goo2"}) ==
              foo_cs_map.end());
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple1.Offset(false), "goo3"}) !=
              foo_cs_map.end());
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple1.Offset(false), "goo4"}) !=
              foo_cs_map.end());
  EXPECT_TRUE(foo_cs_map.find(Callsite{tuple2.Offset(false), "hoo"}) !=
              foo_cs_map.end());
  EXPECT_EQ(hoo_cs_map.size(), 2);
  EXPECT_TRUE(hoo_cs_map.find(Callsite{tuple3.Offset(false), "bar1"}) ==
              hoo_cs_map.end());
  EXPECT_TRUE(hoo_cs_map.find(Callsite{tuple3.Offset(false), "bar2"}) !=
              hoo_cs_map.end());
  EXPECT_TRUE(hoo_cs_map.find(Callsite{tuple3.Offset(false), "bar3"}) !=
              hoo_cs_map.end());
  EXPECT_TRUE(hoo_cs_map.find(Callsite{tuple3.Offset(false), "bar4"}) ==
              hoo_cs_map.end());
}

TEST(AddressConversion, VaddrToOffset) {
  // clang-format off
  // NOLINTBEGIN(whitespace/line_length)
  // Program Headers:
  //   Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  //   INTERP         0x001000 0x0000000000000000 0x0000000000000000 0x00001c 0x00001c R   0x1
  //       [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
  //   LOAD           0x001000 0x0000000000000000 0x0000000000000000 0x001004 0x001004 R   0x1000
  //   LOAD           0x002004 0x0000000000001004 0x0000000000001004 0x02f00c 0x02f00c R E 0x1000
  //   GNU_STACK      0x000000 0x0000000000000000 0x0000000000000000 0x000000 0x000000 RW  0x0
  // NOLINTEND(whitespace/line_length)
  // clang-format on

  SymbolMap symbol_map(absl::StrCat(testing::SrcDir(), kTestDataDir,
                                    "vaddr_offset_conversion_test.elf"));
  symbol_map.ReadLoadableExecSegmentInfo(/*is_kernel=*/false);
  EXPECT_EQ(symbol_map.get_static_vaddr(0x002010), 0x001010);
}

TEST(AddressConversion, OffsetToVaddr) {
  // clang-format off
  // NOLINTBEGIN(whitespace/line_length)
  // Program Headers:
  //   Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  //   INTERP         0x001000 0x0000000000000000 0x0000000000000000 0x00001c 0x00001c R   0x1
  //       [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
  //   LOAD           0x001000 0x0000000000000000 0x0000000000000000 0x001004 0x001004 R   0x1000
  //   LOAD           0x002004 0x0000000000001004 0x0000000000001004 0x02f00c 0x02f00c R E 0x1000
  //   GNU_STACK      0x000000 0x0000000000000000 0x0000000000000000 0x000000 0x000000 RW  0x0
  // NOLINTEND(whitespace/line_length)
  // clang-format on

  SymbolMap symbol_map(absl::StrCat(testing::SrcDir(), kTestDataDir,
                                    "vaddr_offset_conversion_test.elf"));
  symbol_map.ReadLoadableExecSegmentInfo(/*is_kernel=*/false);
  EXPECT_EQ(symbol_map.GetFileOffsetFromStaticVaddr(0x001010), 0x002010);
}

}  // namespace
