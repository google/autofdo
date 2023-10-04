#include "llvm_propeller_profile_computer.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_profile.h"
#include "llvm_propeller_program_cfg.h"
#include "llvm_propeller_statistics.h"
#include "status_provider.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/btree_set.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "third_party/abseil/absl/types/span.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace devtools_crosstool_autofdo {
#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::HasSubstr;
using ::testing::Key;
using ::testing::Not;
using ::testing::Pair;
using ::testing::Pointee;
using ::testing::Property;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

MATCHER_P7(CfgNodeFieldsAre, function_index, bb_index, clone_number, bb_id,
           address, size, freq,
           absl::StrFormat("%s fields {function_index: %d, bb_index: %d, "
                           "clone_number: %d, bb_id: %d, address: 0x%llX, "
                           "size: 0x%llX, frequency: %llu}",
                           negation ? "doesn't have" : "has", function_index,
                           bb_index, clone_number, bb_id, address, size,
                           freq)) {
  return arg.function_index() == function_index && arg.bb_index() == bb_index &&
         arg.clone_number() == clone_number && arg.addr() == address &&
         arg.bb_id() == bb_id && arg.size() == size &&
         arg.CalculateFrequency() == freq;
}

static std::string GetAutoFdoTestDataFilePath(absl::string_view filename) {
  return absl::StrCat(FLAGS_test_srcdir,
                      "/testdata/",
                      filename);
}

TEST(LlvmPropellerProfileComputerTest, CreateCfg) {
  const std::string binary = GetAutoFdoTestDataFilePath("propeller_sample.bin");
  const std::string perfdata =
      GetAutoFdoTestDataFilePath("propeller_sample.perfdata");

  const PropellerOptions options(PropellerOptionsBuilder()
                                     .SetBinaryName(binary)
                                     .AddPerfNames(perfdata)
                                     .SetClusterOutName("dummy.out"));

  DefaultStatusProvider status("generate profile");
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<ProgramCfg> program_cfg,
                       profile_computer->GetProgramCfg(&status));
  EXPECT_TRUE(status.IsDone());
  auto cfgs_by_name = program_cfg->cfgs_by_name();
  ASSERT_THAT(cfgs_by_name,
              UnorderedElementsAre(Key("main"), Key("compute_flag"),
                                   Key("this_is_very_code")));

  const ControlFlowGraph &main = *cfgs_by_name.at("main");
  const ControlFlowGraph &compute_flag = *cfgs_by_name.at("compute_flag");

  // Examine main nodes / edges.
  EXPECT_THAT(main.nodes(), SizeIs(9));

  EXPECT_THAT(main.inter_edges(),
              Contains(Pointee(AllOf(Property(&CFGEdge::IsCall, Eq(true)),
                                     Property(&CFGEdge::weight, Gt(100))))));
  EXPECT_THAT(compute_flag.inter_edges(),
              Contains(Pointee(AllOf(Property(&CFGEdge::IsReturn, Eq(true)),
                                     Property(&CFGEdge::weight, Gt(100))))));
}

TEST(LlvmPropellerProfileComputerTest, CreateProgramCfg) {
  const PropellerOptions options(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample.bin"))
          .AddPerfNames(GetAutoFdoTestDataFilePath("propeller_sample.perfdata"))
          .SetClusterOutName("dummy.out"));
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<ProgramCfg> program_cfg,
                       profile_computer->GetProgramCfg());
  auto cfgs_by_name = program_cfg->cfgs_by_name();
  ASSERT_THAT(cfgs_by_name, Contains(Key("main")));
  const ControlFlowGraph &main_cfg = *cfgs_by_name.at("main");
  ASSERT_EQ(main_cfg.GetPrimaryName().str(), "main");
  int function_index = main_cfg.function_index();
  // Regenerating propeller_sample.bin and propeller_sample.perfdata may trigger
  // a change here.
  // Use `llvm-objdump -d -symbolize-operands propeller_sample.bin` to capture
  // basic block address and sizes, and use the create_llvm_prof option
  // `--propeller_verbose_cluster_output` to get the node frequencies.
  EXPECT_THAT(
      main_cfg.nodes(),
      ElementsAre(
          Pointee(CfgNodeFieldsAre(function_index, 0, 0, 0, 0x1820, 0x30, 1)),
          Pointee(
              CfgNodeFieldsAre(function_index, 1, 0, 1, 0x1850, 0xD, 646312)),
          Pointee(
              CfgNodeFieldsAre(function_index, 2, 0, 2, 0x185D, 0x24, 660951)),
          Pointee(
              CfgNodeFieldsAre(function_index, 3, 0, 3, 0x1881, 0x2E, 191807)),
          Pointee(
              CfgNodeFieldsAre(function_index, 4, 0, 4, 0x18AF, 0x1A, 622992)),
          Pointee(
              CfgNodeFieldsAre(function_index, 5, 0, 5, 0x18C9, 0x34, 3892)),
          Pointee(
              CfgNodeFieldsAre(function_index, 6, 0, 6, 0x18FD, 0x5, 634928)),
          Pointee(
              CfgNodeFieldsAre(function_index, 7, 0, 7, 0x1902, 0xE, 646311)),
          Pointee(CfgNodeFieldsAre(function_index, 8, 0, 8, 0x1910, 0x8, 0))));
}

TEST(LlvmPropellerProfileComputerTest, CheckStatsSanity) {
  const PropellerOptions options(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample.bin"))
          .AddPerfNames(GetAutoFdoTestDataFilePath("propeller_sample.perfdata"))
          .SetClusterOutName("dummy.out"));
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<ProgramCfg> program_cfg,
                       profile_computer->GetProgramCfg());
  const PropellerStats &stats = profile_computer->stats();
  EXPECT_EQ(stats.cfgs_created, 3);
  EXPECT_EQ(stats.total_edges_created(), 17);
  EXPECT_EQ(stats.total_edge_weight_created(), 5850315);
  EXPECT_EQ(stats.nodes_created, 14);
  EXPECT_EQ(stats.binary_mmap_num, 3);
  EXPECT_EQ(stats.duplicate_symbols, 0);
  EXPECT_EQ(stats.hot_basic_blocks, 13);
  EXPECT_EQ(stats.hot_empty_basic_blocks, 0);
  EXPECT_EQ(stats.cfgs_with_hot_landing_pads, 0);
}

TEST(LlvmPropellerProfileComputerTest, TestSourceDrift1) {
  // "propeller_sample_O0.bin" is used against propeller_sample.perfdata which
  // is collected from an "-O2" binary, this is a source drift.
  const PropellerOptions options(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample_O0.bin"))
          .AddPerfNames(GetAutoFdoTestDataFilePath("propeller_sample.perfdata"))
          .SetIgnoreBuildId(true)
          .SetProfiledBinaryName("propeller_sample.bin.gen")
          .SetClusterOutName("dummy.out"));
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  EXPECT_THAT(
      profile_computer->GetProgramCfg(),
      StatusIs(
          absl::StatusCode::kInternal,
          HasSubstr(
              "Unable to create edges from LBR profile: Too many jumps into "
              "middle of basic blocks detected, probably because of source "
              "drift")));
}

TEST(LlvmPropellerProfileComputerTest, TestMultiplePerfDataFiles) {
  auto get_program_cfg =
      [](absl::Span<const std::string> perfs) -> std::unique_ptr<ProgramCfg> {
    PropellerOptionsBuilder options_builder;
    options_builder.SetBinaryName(
        GetAutoFdoTestDataFilePath("propeller_sample_1.bin"));
    for (const std::string &perf : perfs) options_builder.AddPerfNames(perf);
    const PropellerOptions options(options_builder);
    absl::StatusOr<std::unique_ptr<PropellerProfileComputer>> profile_Computer =
        PropellerProfileComputer::Create(options);
    CHECK_OK(profile_Computer);
    auto program_cfg = (*profile_Computer)->GetProgramCfg();
    return *std::move(program_cfg);
  };

  std::string perf1 =
      GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1");
  std::string perf2 =
      GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata2");
  auto program_cfg1 = get_program_cfg({perf1});
  auto program_cfg2 = get_program_cfg({perf2});
  auto program_cfg12 = get_program_cfg({perf1, perf2});
  auto cfgs1_by_name = program_cfg1->cfgs_by_name();
  auto cfgs2_by_name = program_cfg2->cfgs_by_name();
  auto cfgs12_by_name = program_cfg12->cfgs_by_name();

  // The run for perf2 is a superset of the run for perf1. Thus cfg2 has more
  // nodes (hot nodes) than cfg1. However, perf1 + perf2 includes as many hot
  // nodes as perf2.
  const ControlFlowGraph &cfg1 = *cfgs1_by_name.at("main");
  const ControlFlowGraph &cfg2 = *cfgs2_by_name.at("main");
  const ControlFlowGraph &cfg12 = *cfgs12_by_name.at("main");
  EXPECT_GE(cfg2.nodes().size(), cfg1.nodes().size());
  EXPECT_EQ(cfg2.nodes().size(), cfg12.nodes().size());

  absl::btree_set<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>>
      edge_set1;
  absl::btree_set<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>>
      edge_set2;
  absl::btree_set<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>>
      edge_set12;
  auto initialize_edge_set =
      [](const ControlFlowGraph &cfg,
         absl::btree_set<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>>
             &edge_set) {
        for (auto &edge : cfg.intra_edges())
          edge_set.emplace(edge->src()->inter_cfg_id(),
                           edge->sink()->inter_cfg_id());
      };
  initialize_edge_set(cfg1, edge_set1);
  initialize_edge_set(cfg2, edge_set2);
  initialize_edge_set(cfg12, edge_set12);

  // Perfdata 1 & 2 contain different edge sets, this is because perfdata2 is
  // collected with an additional test run argument that directs the binary to
  // run in a different code path.  Refer to "propeller_sample_1.c"  block under
  // "if (argc > 1) {".

  // edges constructed from perf1 differs from those constructed from perf2.
  EXPECT_NE(edge_set1, edge_set2);

  absl::btree_set<std::pair<CFGNode::InterCfgId, CFGNode::InterCfgId>> union12;
  absl::c_set_union(edge_set1, edge_set2,
                    std::inserter(union12, union12.begin()));
  // The union of edges constructed from perf1 and perf2 separately equals to
  // those constructed from perf1 and perf2 together.
  EXPECT_EQ(union12, edge_set12);

  auto accumulator = [](uint64_t acc,
                        const std::unique_ptr<CFGEdge> &e) -> uint64_t {
    return acc + e->weight();
  };
  uint64_t weight1 = absl::c_accumulate(cfg1.intra_edges(), 0, accumulator);
  uint64_t weight2 = absl::c_accumulate(cfg2.intra_edges(), 0, accumulator);
  uint64_t weight12 = absl::c_accumulate(cfg12.intra_edges(), 0, accumulator);
  // The sum of weights collected from perf file 1 & 2 separately equals to the
  // sum of weights collected from perf file 1 & 2 in oneshot.
  EXPECT_EQ(weight1 + weight2, weight12);
}

TEST(LlvmPropellerProfileComputerTest, CreateProgramCfgOnlyForHotFunctions) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample_1.bin"))
          .AddPerfNames(
              GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1")));
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<ProgramCfg> program_cfg,
                       profile_computer->GetProgramCfg());

  // "sample1_func" is cold, and should not have a CFG, but "main" is a hot
  // function and should have a CFG.
  EXPECT_THAT(program_cfg->cfgs_by_name(),
              AllOf(Not(Contains(Key("sample1_func"))), Contains(Key("main"))));
}

TEST(LlvmPropellerProfileComputerTest, TestCheckLbrAggregationOk) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample_1.bin"))
          .AddPerfNames(
              GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1")));
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  EXPECT_OK(profile_computer->ParsePerfData());

  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.could_not_disassemble.absolute,
            0);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.may_affect_control_flow.absolute,
            9);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.cant_affect_control_flow.absolute,
            0);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.could_not_disassemble.weighted,
            0);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.may_affect_control_flow.weighted,
            119419);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.cant_affect_control_flow.weighted,
            0);
}

TEST(LlvmPropellerProfileComputerTest, TestCheckLbrAggregationFail) {
  // Providing a binary/perfdata pair that do not match will cause error:
  // "100.00% branches's source addresses failed to disassemble. This indicates
  // an address translation error caused either by a bug in the tool or the
  // provided binary does not match perf.data file."
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("ml_test.binary"))
          .SetProfiledBinaryName("propeller_sample_1.bin.gen")
          .AddPerfNames(
              GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1")));
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  EXPECT_OK(profile_computer->ParsePerfData());

  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.could_not_disassemble.absolute,
            9);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.may_affect_control_flow.absolute,
            0);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.cant_affect_control_flow.absolute,
            0);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.could_not_disassemble.weighted,
            119419);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.may_affect_control_flow.weighted,
            0);
  EXPECT_EQ(profile_computer->stats()
                .disassembly_stats.cant_affect_control_flow.weighted,
            0);
}

TEST(LlvmPropellerProfileComputerTest, NonTextSectionsDropped) {
  const PropellerOptions options(PropellerOptionsBuilder()
                                     .SetBinaryName(GetAutoFdoTestDataFilePath(
                                         "propeller_sample_section.bin"))
                                     .AddPerfNames(GetAutoFdoTestDataFilePath(
                                         "propeller_sample_section.perfdata"))
                                     .SetClusterOutName("dummy.out"));
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  ASSERT_OK_AND_ASSIGN(PropellerProfile propeller_profile,
                       profile_computer->ComputeProfile());
  EXPECT_THAT(propeller_profile.functions_cluster_info_by_section_name,
              ElementsAre(Pair(".text", SizeIs(2))));
}

TEST(LlvmPropellerProfileComputerTest, NonTextSectionsKept) {
  const PropellerOptions options(PropellerOptionsBuilder()
                                     .SetBinaryName(GetAutoFdoTestDataFilePath(
                                         "propeller_sample_section.bin"))
                                     .AddPerfNames(GetAutoFdoTestDataFilePath(
                                         "propeller_sample_section.perfdata"))
                                     .SetClusterOutName("dummy.out")
                                     .SetFilterNonTextFunctions(false));
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(options));
  ASSERT_OK_AND_ASSIGN(PropellerProfile propeller_profile,
                       profile_computer->ComputeProfile());
  EXPECT_THAT(propeller_profile.functions_cluster_info_by_section_name,
              ElementsAre(Pair(".anycall.anysection", SizeIs(1)),
                          Pair(".othercall.othersection", SizeIs(1)),
                          Pair(".text", SizeIs(2))));
}

}  // namespace
}  // namespace devtools_crosstool_autofdo
