#include "llvm_propeller_program_cfg.h"

#include <memory>
#include <string>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_mock_program_cfg_builder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/testing/status_matchers.h"

namespace devtools_crosstool_autofdo {
namespace {

using ::testing::ElementsAre;
using ::testing::Gt;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Pair;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

// This test checks that the mock can load a CFG from the serialized format
// correctly.
TEST(LlvmPropellerWholeProgramCfgInfo, CreateCfgInfoFromProto) {
  const std::string protobuf_input = absl::StrCat(
      ::testing::SrcDir(),
      "/testdata/propeller_sample.protobuf");

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<ProtoProgramCfg> proto_program_cfg,
                       BuildFromCfgProtoPath(protobuf_input));
  EXPECT_THAT(proto_program_cfg->program_cfg().cfgs_by_index(), SizeIs(Gt(10)));

  // Check some inter-func edge is valid.
  const ControlFlowGraph *main =
      proto_program_cfg->program_cfg().GetCfgByIndex(9);
  ASSERT_NE(main, nullptr);
  EXPECT_EQ(main->GetPrimaryName(), "main");
  EXPECT_THAT(main->inter_edges(), Not(IsEmpty()));
  CFGEdge &edge = *(main->inter_edges().front());
  EXPECT_EQ(edge.src()->function_index(), main->function_index());
  EXPECT_NE(edge.sink()->function_index(), main->function_index());
  // The same "edge" instance exists both in src->inter_outs_ and
  // sink->inter_ins_.
  EXPECT_NE(absl::c_find(edge.sink()->inter_ins(), &edge),
            edge.sink()->inter_ins().end());
}

TEST(LlvmPropellerWholeProgramCfgInfo, GetNodeFrequencyThreshold) {
  std::unique_ptr<ProgramCfg> program_cfg = BuildFromCfgArg(
      {.cfg_args = {{".foo_section",
                     0,
                     "foo",
                     {{0x1000, 0, 0x10}, {0x1010, 1, 0x7}, {0x1020, 2, 0xa}},
                     {{0, 2, 8, CFGEdge::Kind::kBranchOrFallthough}}},
                    {".bar_section",
                     1,
                     "bar",
                     {{0x2000, 0, 0x20}, {0x2020, 1, 0x10}},
                     {}},
                    {".baz_section",
                     2,
                     "baz",
                     {{0x3000, 0, 0x10}, {0x3010, 1, 0x5}},
                     {}}},
       .inter_edge_args = {{0, 0, 1, 0, 10, CFGEdge::Kind::kCall},
                           {0, 0, 2, 0, 5, CFGEdge::Kind::kCall},
                           {0, 1, 1, 0, 2, CFGEdge::Kind::kCall}}});
  EXPECT_EQ(program_cfg->GetNodeFrequencyThreshold(100), 12);
  EXPECT_EQ(program_cfg->GetNodeFrequencyThreshold(80), 10);
  EXPECT_EQ(program_cfg->GetNodeFrequencyThreshold(50), 5);
  EXPECT_EQ(program_cfg->GetNodeFrequencyThreshold(10), 0);
  EXPECT_EQ(program_cfg->GetNodeFrequencyThreshold(1), 0);
}

TEST(LlvmPropellerWholeProgramCfgInfo, GetHotJoinNodes) {
  std::unique_ptr<ProgramCfg> program_cfg = BuildFromCfgArg(
      {.cfg_args = {
           {".foo_section",
            0,
            "foo",
            {{0x1000, 0, 0x10},
             {0x1010, 1, 0x7},
             {0x102a, 2, 0x4},
             {0x1030, 3, 0x8}},
            {{0, 1, 10, CFGEdge::Kind::kBranchOrFallthough},
             {0, 3, 20, CFGEdge::Kind::kBranchOrFallthough},
             {1, 2, 30, CFGEdge::Kind::kBranchOrFallthough},
             {2, 1, 40, CFGEdge::Kind::kBranchOrFallthough}}},
           {".bar_section",
            1,
            "bar",
            {{0x2000, 0, 0x10}, {0x2010, 1, 0x7}, {0x202a, 2, 0x4}},
            {{0, 2, 50, CFGEdge::Kind::kBranchOrFallthough},
             {0, 1, 30, CFGEdge::Kind::kBranchOrFallthough},
             {1, 2, 30, CFGEdge::Kind::kBranchOrFallthough}}},
       }});
  EXPECT_THAT(program_cfg->GetHotJoinNodes(30, 20),
              UnorderedElementsAre(Pair(1, ElementsAre(2))));
  EXPECT_THAT(
      program_cfg->GetHotJoinNodes(30, 10),
      UnorderedElementsAre(Pair(0, ElementsAre(1)), Pair(1, ElementsAre(2))));
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
