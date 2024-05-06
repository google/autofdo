#include "llvm_propeller_program_cfg_builder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "branch_aggregation.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_program_cfg.h"
#include "llvm_propeller_statistics.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/testing/status_matchers.h"

namespace devtools_crosstool_autofdo {
namespace {
using ::testing::ElementsAre;
using ::testing::Key;
using ::testing::UnorderedElementsAre;

TEST(ProgramCfgBuilderTest, BuildProgramCfgOverProgramCfg) {
  const std::string binary =
      absl::StrCat(::testing::SrcDir(),
                   "/testdata/"
                   "propeller_bimodal_sample.bin");
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(binary));

  // Two `BranchAggregation` profiles representing two different runs of the
  // program.
  BranchAggregation branch_aggregation1(
      {.branch_counters = {{{0x186a, 0x1730}, 1000},
                           {{0x1897, 0x1860}, 1000},
                           {{0x1802, 0x184b}, 10}},
       .fallthrough_counters = {{{0x1730, 0x1782}, 1000},
                                {{0x1860, 0x186a}, 1000},
                                {{0x17f0, 0x1802}, 10}}});
  absl::flat_hash_set<uint64_t> unique_addresses1 = {
      0x1730, 0x1782, 0x17f0, 0x1802, 0x184b, 0x1860, 0x186a, 0x1897};

  BranchAggregation branch_aggregation2(
      {.branch_counters = {{{0x181a, 0x1790}, 500},
                           {{0x17ea, 0x181f}, 500},
                           {{0x1847, 0x1810}, 500}},
       .fallthrough_counters = {{{{0x1790, 0x17ea}, 500},
                                 {{0x1810, 0x181a}, 500},
                                 {{0x17f0, 0x181a}, 5}}}});
  absl::flat_hash_set<uint64_t> unique_addresses2 = {
      0x1790, 0x17ea, 0x17f0, 0x1810, 0x181a, 0x181f, 0x1847};

  PropellerOptions options;

  // Build `program_cfg1` from `branch_aggregation1` and check the hot node
  // frequencies.
  PropellerStats stats1;
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryAddressMapper> address_mapper1,
                       BuildBinaryAddressMapper(options, *binary_content,
                                                stats1, &unique_addresses1));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<ProgramCfg> program_cfg1,
                       ProgramCfgBuilder(address_mapper1.get(), stats1)
                           .Build(branch_aggregation1, /*file_content=*/nullptr,
                                  /*addr2cu=*/nullptr));
  auto cfgs1 = program_cfg1->cfgs_by_name();
  ASSERT_THAT(cfgs1, UnorderedElementsAre(Key("foo"), Key("compute")));
  EXPECT_THAT(cfgs1.at("foo")->GetNodeFrequencies(), ElementsAre(1000));
  EXPECT_THAT(cfgs1.at("compute")->GetNodeFrequencies(),
              ElementsAre(10, 0, 0, 10, 1000, 0));

  // Build `program_cfg2` from `branch_aggregation2` and check the hot node
  // frequencies.
  PropellerStats stats2;
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryAddressMapper> address_mapper2,
                       BuildBinaryAddressMapper(options, *binary_content,
                                                stats2, &unique_addresses2));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<ProgramCfg> program_cfg2,
                       ProgramCfgBuilder(address_mapper2.get(), stats2)
                           .Build(branch_aggregation2, nullptr, nullptr));
  auto cfgs2 = program_cfg2->cfgs_by_name();
  ASSERT_THAT(cfgs2, UnorderedElementsAre(Key("compute"), Key("bar")));
  EXPECT_THAT(cfgs2.at("bar")->GetNodeFrequencies(), ElementsAre(500));
  EXPECT_THAT(cfgs2.at("compute")->GetNodeFrequencies(),
              ElementsAre(5, 5, 505, 0, 0, 0));

  // Build `program_cfg3` on top of `program_cfg1` and from
  // `branch_aggregation2`.
  PropellerStats stats3;
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ProgramCfg> program_cfg3,
      ProgramCfgBuilder(address_mapper2.get(), stats3, std::move(program_cfg1))
          .Build(branch_aggregation2, nullptr, nullptr));
  // Check that `program_cfg3` is the merge of `program_cfg1` and
  // `program_cfg2`.
  auto cfgs3 = program_cfg3->cfgs_by_name();
  ASSERT_THAT(cfgs3,
              UnorderedElementsAre(Key("bar"), Key("compute"), Key("foo")));
  EXPECT_THAT(cfgs3.at("compute")->GetNodeFrequencies(),
              ElementsAre(15, 5, 505, 10, 1000, 0));
  EXPECT_THAT(cfgs3.at("foo")->GetNodeFrequencies(), ElementsAre(1000));
  EXPECT_THAT(cfgs3.at("bar")->GetNodeFrequencies(), ElementsAre(500));

  // Build `program_cfg4` on top of `program_cfg2` and from
  // `branch_aggregation1`.
  PropellerStats stats4;
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ProgramCfg> program_cfg4,
      ProgramCfgBuilder(address_mapper1.get(), stats4, std::move(program_cfg2))
          .Build(branch_aggregation1, nullptr, nullptr));
  // Check that `program_cfg4` is the merge of `program_cfg1` and
  // `program_cfg2`, exactly the same as `program_cfg3`.
  auto cfgs4 = program_cfg4->cfgs_by_name();
  ASSERT_THAT(cfgs4,
              UnorderedElementsAre(Key("bar"), Key("compute"), Key("foo")));
  EXPECT_THAT(cfgs3.at("compute")->GetNodeFrequencies(),
              ElementsAre(15, 5, 505, 10, 1000, 0));
  EXPECT_THAT(cfgs3.at("foo")->GetNodeFrequencies(), ElementsAre(1000));
  EXPECT_THAT(cfgs3.at("bar")->GetNodeFrequencies(), ElementsAre(500));
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
