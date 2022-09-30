#include "llvm_propeller_whole_program_info.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_mock_whole_program_info.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "perfdata_reader.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Path.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define ASSERT_OK(exp) ASSERT_TRUE((exp).ok())
#define EXPECT_OK(exp) EXPECT_TRUE((exp).ok())

namespace {

using ::devtools_crosstool_autofdo::BranchDirection;
using ::devtools_crosstool_autofdo::CfgCreationMode;
using ::devtools_crosstool_autofdo::CFGEdge;
using ::devtools_crosstool_autofdo::CFGNode;
using ::devtools_crosstool_autofdo::ControlFlowGraph;
using ::devtools_crosstool_autofdo::MockPropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::MultiStatusProvider;
using ::devtools_crosstool_autofdo::PropellerOptions;
using ::devtools_crosstool_autofdo::PropellerOptionsBuilder;
using ::devtools_crosstool_autofdo::PropellerWholeProgramInfo;

using ::testing::_;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::FieldsAre;
using ::testing::IsEmpty;
using ::testing::Key;
using ::testing::Not;
using ::testing::Optional;
using ::testing::Pair;
using ::testing::Pointee;
using ::testing::ResultOf;
using ::testing::UnorderedElementsAre;

using ::llvm::object::BBAddrMap;

MATCHER_P2(BbAddrMapIs, function_address_matcher, bb_entries_matcher, "") {
  return ExplainMatchResult(function_address_matcher, arg.Addr,
                            result_listener) &&
         ExplainMatchResult(bb_entries_matcher, arg.BBEntries, result_listener);
}

MATCHER_P6(
    CfgNodeFieldsAre, symbol_ordinal, address, bb_index, freq, size, cfg,
    absl::StrFormat("%s fields {symbol_ordinal: %llu, address: 0x%llX, "
                    "bb_index: %d, frequency: %llu, size: 0x%llX, CFG: %p}",
                    negation ? "doesn't have" : "has", symbol_ordinal, address,
                    bb_index, freq, size, cfg)) {
  return arg.symbol_ordinal() == symbol_ordinal && arg.addr() == address &&
         arg.bb_index() == bb_index && arg.freq() == freq &&
         arg.size() == size && arg.cfg() == cfg;
}

static std::string GetAutoFdoTestDataFilePath(const std::string &filename) {
  const std::string testdata_filepath =
      FLAGS_test_srcdir +
      "/testdata/" +
      filename;
  return testdata_filepath;
}

static absl::flat_hash_set<llvm::StringRef> GetAllFunctionNamesFromSymtab(
    const typename PropellerWholeProgramInfo::SymTabTy &symtab) {
  absl::flat_hash_set<llvm::StringRef> all_functions;
  for (const auto &symtab_entry : symtab)
    for (const auto &symref : symtab_entry.second)
      all_functions.insert(
          llvm::cantFail(llvm::object::ELFSymbolRef(symref).getName()));
  return all_functions;
}

static absl::flat_hash_map<llvm::StringRef, BBAddrMap>
GetBBAddrMapByFunctionName(const PropellerWholeProgramInfo &wpi) {
  absl::flat_hash_map<llvm::StringRef, BBAddrMap> bb_addr_map_by_func_name;
  for (const auto &[function_index, function_aliases] :
       wpi.function_index_to_names_map()) {
    for (llvm::StringRef alias : function_aliases)
      bb_addr_map_by_func_name.insert(
          {alias, wpi.bb_addr_map()[function_index]});
  }
  return bb_addr_map_by_func_name;
}

TEST(LlvmPropellerWholeProgramInfo, CreateCFG) {
  const std::string binary = GetAutoFdoTestDataFilePath("propeller_sample.bin");
  const std::string perfdata =
      GetAutoFdoTestDataFilePath("propeller_sample.perfdata");

  const PropellerOptions options(
      PropellerOptionsBuilder()
          .SetBinaryName(binary)
          .AddPerfNames(perfdata)
          .SetClusterOutName("dummy.out")
          .SetProfiledBinaryName("propeller_sample.bin"));

  MultiStatusProvider status("generate profile");
  auto wpi = PropellerWholeProgramInfo::Create(options, &status);
  ASSERT_NE(wpi, nullptr);
  EXPECT_EQ(status.GetProgress(), 1);
  ASSERT_OK(wpi->CreateCfgs(CfgCreationMode::kAllFunctions));
  EXPECT_TRUE(status.IsDone());
  // Test resources are released after CreateCFG.
  EXPECT_THAT(wpi->binary_mmaps(), IsEmpty());

  const ControlFlowGraph *main = wpi->FindCfg("main");
  EXPECT_TRUE(main);
  const ControlFlowGraph *compute_flag = wpi->FindCfg("compute_flag");
  EXPECT_TRUE(compute_flag);
  const ControlFlowGraph *sample1_func = wpi->FindCfg("sample1_func");
  EXPECT_TRUE(sample1_func);
  const ControlFlowGraph *this_is_vey_code = wpi->FindCfg("this_is_very_code");
  EXPECT_TRUE(this_is_vey_code);

  // Examine main nodes / edges.
  EXPECT_GE(main->nodes().size(), 4);

  EXPECT_GE(main->inter_edges().size(), 1);
  EXPECT_TRUE(main->inter_edges().front()->IsCall());
  EXPECT_GT(main->inter_edges().front()->weight(), 100);

  EXPECT_GE(compute_flag->inter_edges().size(), 1);
  EXPECT_TRUE(compute_flag->inter_edges().front()->IsReturn());
  EXPECT_GT(compute_flag->inter_edges().front()->weight(), 100);
}

// This test checks that the mock can load a CFG from the serialized format
// correctly.
TEST(LlvmPropellerMockWholeProgramInfo, CreateCFGFromProto) {
  const std::string protobuf_input =
      GetAutoFdoTestDataFilePath("propeller_sample.protobuf");
  const PropellerOptions options(
      PropellerOptionsBuilder().AddPerfNames(protobuf_input));
  MockPropellerWholeProgramInfo wpi(options);
  ASSERT_OK(wpi.CreateCfgs(CfgCreationMode::kAllFunctions));

  EXPECT_GT(wpi.cfgs().size(), 10);

  // Check some inter-func edge is valid.
  const ControlFlowGraph *main = wpi.FindCfg("main");
  EXPECT_NE(main, (nullptr));
  EXPECT_THAT(main->inter_edges(), Not(IsEmpty()));
  CFGEdge &edge = *(main->inter_edges().front());
  EXPECT_EQ(edge.src()->cfg(), main);
  EXPECT_NE(edge.sink()->cfg(), main);
  // The same "edge" instance exists both in src->inter_outs_ and
  // sink->inter_ins_.
  EXPECT_NE(std::find(edge.sink()->inter_ins().begin(),
                      edge.sink()->inter_ins().end(), &edge),
            edge.sink()->inter_ins().end());
}

TEST(LlvmPropellerWholeProgramInfoBbAddrMapTest, BbAddrMapExist) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  auto wpi = PropellerWholeProgramInfo::Create(PropellerOptions(
      PropellerOptionsBuilder().SetBinaryName(binary).SetClusterOutName(
          "dummy.out")));
  ASSERT_NE(nullptr, wpi) << "Could not initialize whole program info";
  EXPECT_OK(wpi->ReadBinaryInfo());
}

TEST(LlvmPropellerWholeProgramInfoBbAddrTest, BbAddrMapReadSymbolTable) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  auto wpi = PropellerWholeProgramInfo::Create(PropellerOptions(
      PropellerOptionsBuilder().SetBinaryName(binary).SetClusterOutName(
          "dummy.out")));
  ASSERT_NE(nullptr, wpi) << "Could not initialize whole program info";
  ASSERT_OK(wpi->ReadBinaryInfo());
  auto &symtab = wpi->symtab();
  bool found = false;
  for (auto &p : symtab) {
    for (auto &symref : p.second) {
      if (llvm::cantFail(symref.getName()) == "sample1_func")
        found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(LlvmPropellerWholeProgramInfoBbAddrMapTest, SkipEntryIfSymbolNotInSymtab) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_from_icu_genrb");
  auto wpi = PropellerWholeProgramInfo::Create(PropellerOptions(
      PropellerOptionsBuilder().SetBinaryName(binary).SetClusterOutName(
          "dummy.out")));
  ASSERT_NE(nullptr, wpi) << "Could not initialize whole program info";
  ASSERT_OK(wpi->ReadBinaryInfo());
  EXPECT_THAT(wpi->SelectFunctions(CfgCreationMode::kAllFunctions, nullptr),
              Not(IsEmpty()));
  EXPECT_GT(wpi->stats().bbaddrmap_function_does_not_have_symtab_entry, 1);
}

TEST(LlvmPropellerWholeProgramInfoBbAddrMapTest, ReadBbAddrMap) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  auto wpi = PropellerWholeProgramInfo::Create(PropellerOptions(
      PropellerOptionsBuilder().SetBinaryName(binary).SetClusterOutName(
          "dummy.out")));
  ASSERT_NE(nullptr, wpi) << "Could not initialize whole program info";
  ASSERT_OK(wpi->ReadBinaryInfo());
  EXPECT_THAT(wpi->SelectFunctions(CfgCreationMode::kAllFunctions, nullptr),
              Not(IsEmpty()));
  auto bb_addr_map_by_func_name = GetBBAddrMapByFunctionName(*wpi);
  EXPECT_THAT(bb_addr_map_by_func_name,
              Contains(Pair("compute_flag",
                            Field(&BBAddrMap::BBEntries, Not(IsEmpty())))));
  EXPECT_THAT(
      GetBBAddrMapByFunctionName(*wpi),
      UnorderedElementsAre(
          Pair("main",
               FieldsAre(0x11e0,
                         ElementsAre(
                             FieldsAre(0x0, 0x30, false, false, false, false),
                             FieldsAre(0x30, 0xD, false, false, false, false),
                             FieldsAre(0x3D, 0x25, false, false, false, false),
                             FieldsAre(0x62, 0x2E, false, false, false, false),
                             FieldsAre(0x90, 0x1D, false, false, false, false),
                             FieldsAre(0xAD, 0x37, false, false, false, false),
                             FieldsAre(0xE4, 0x5, false, false, false, false),
                             FieldsAre(0xE9, 0xE, false, false, false, false),
                             FieldsAre(0xF7, 0x8, true, false, false, false)))),
          Pair("sample1_func",
               FieldsAre(0x11D0, ElementsAre(FieldsAre(0x0, 0xB, true, false,
                                                       false, false)))),
          Pair("compute_flag",
               FieldsAre(0x1190,
                         ElementsAre(
                             FieldsAre(0x0, 0x1B, false, false, false, false),
                             FieldsAre(0x1B, 0xE, false, false, false, false),
                             FieldsAre(0x29, 0x7, false, false, false, false),
                             FieldsAre(0x30, 0x5, true, false, false, false)))),
          Pair("this_is_very_code",
               FieldsAre(0x1130, ElementsAre(FieldsAre(0x0, 0x5d, true, false,
                                                       false, false))))));
}

TEST(LlvmPropellerWholeProgramInfoBbInfoTest, CreateCfgsFromBbInfo) {
  const PropellerOptions options(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample.bin"))
          .AddPerfNames(GetAutoFdoTestDataFilePath("propeller_sample.perfdata"))
          .SetKeepFrontendIntermediateData(true)
          .SetClusterOutName("dummy.out")
          .SetProfiledBinaryName("propeller_sample.bin"));
  auto whole_program_info = PropellerWholeProgramInfo::Create(options);
  ASSERT_NE(whole_program_info, nullptr);
  ASSERT_OK(whole_program_info->CreateCfgs(CfgCreationMode::kAllFunctions));
  auto &cfgs = whole_program_info->cfgs();
  ASSERT_THAT(cfgs, Contains(Pair("main", _)));
  const ControlFlowGraph &main_cfg = *cfgs.at("main");
  EXPECT_THAT(
      main_cfg.nodes(),
      ElementsAre(
          Pointee(CfgNodeFieldsAre(6, 0x11E0, 0, 1, 0x30, &main_cfg)),
          Pointee(CfgNodeFieldsAre(7, 0x1210, 1, 22714, 0xD, &main_cfg)),
          Pointee(CfgNodeFieldsAre(8, 0x121D, 2, 23039, 0x25, &main_cfg)),
          Pointee(CfgNodeFieldsAre(9, 0x1242, 3, 0, 0x6D, &main_cfg)),
          Pointee(CfgNodeFieldsAre(10, 0x1270, 4, 21603, 0x1D, &main_cfg)),
          Pointee(CfgNodeFieldsAre(12, 0x12C4, 6, 21980, 0x5, &main_cfg)),
          Pointee(CfgNodeFieldsAre(13, 0x12C9, 7, 22714, 0xE, &main_cfg))));
}

TEST(LlvmPropellerWholeProgramInfoBbInfoTest, CheckStatsSanity) {
  const PropellerOptions options(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample.bin"))
          .AddPerfNames(GetAutoFdoTestDataFilePath("propeller_sample.perfdata"))
          .SetClusterOutName("dummy.out")
          .SetKeepFrontendIntermediateData(true)
          .SetProfiledBinaryName("propeller_sample.bin"));
  auto whole_program_info = PropellerWholeProgramInfo::Create(options);
  ASSERT_NE(whole_program_info, nullptr);
  ASSERT_OK(whole_program_info->CreateCfgs(CfgCreationMode::kAllFunctions));
  EXPECT_GT(whole_program_info->stats().cfgs_created, 3);
  EXPECT_GT(whole_program_info->stats().total_edges_created(), 3);
  EXPECT_GT(whole_program_info->stats().total_edge_weight_created(), 3);
  EXPECT_GT(whole_program_info->stats().nodes_created, 8);
  EXPECT_GT(whole_program_info->stats().binary_mmap_num, 1);
  EXPECT_EQ(whole_program_info->stats().duplicate_symbols, 0);
}

TEST(LlvmPropellerWholeProgramInfoBbInfoTest, TestSourceDrift1) {
  // "propeller_sample_O0.bin" is used against propeller_sample.perfdata which
  // is collected from an "-O2" binary, this is a source drift.
  const PropellerOptions options(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample_O0.bin"))
          .AddPerfNames(GetAutoFdoTestDataFilePath("propeller_sample.perfdata"))
          .SetClusterOutName("dummy.out")
          .SetKeepFrontendIntermediateData(true)
          .SetProfiledBinaryName("propeller_sample.bin"));
  auto whole_program_info = PropellerWholeProgramInfo::Create(options);
  ASSERT_EQ(whole_program_info, nullptr);
}

TEST(LlvmPropellerWholeProgramInfoBbInfoTest, TestMultiplePerfDataFiles) {
  auto init_whole_program_info = [](const std::vector<std::string> &perfs)
      -> std::unique_ptr<PropellerWholeProgramInfo> {
    PropellerOptionsBuilder options_builder;
    options_builder.SetBinaryName(
        GetAutoFdoTestDataFilePath("propeller_sample_1.bin"));
    std::for_each(perfs.begin(), perfs.end(),
                  [&options_builder](const std::string &perf) {
                    options_builder.AddPerfNames(perf);
                  });
    const PropellerOptions options(options_builder);
    std::unique_ptr<PropellerWholeProgramInfo> wpi =
        PropellerWholeProgramInfo::Create(options);
    EXPECT_NE(wpi.get(), nullptr);
    EXPECT_OK(wpi->CreateCfgs(CfgCreationMode::kAllFunctions));
    return wpi;
  };

  std::string perf1 =
      GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1");
  std::string perf2 =
      GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata2");
  auto wpi1 = init_whole_program_info(std::vector<std::string>{perf1});
  auto wpi2 = init_whole_program_info(std::vector<std::string>{perf2});
  auto wpi12 = init_whole_program_info(std::vector<std::string>{perf1, perf2});

  // The run for perf2 is a superset of the run for perf1. Thus cfg2 has more
  // nodes (hot nodes) than cfg1. However, perf1 + perf2 includes as many hot
  // nodes as perf2.
  ControlFlowGraph *cfg1 = wpi1->cfgs().at("main").get();
  ControlFlowGraph *cfg2 = wpi2->cfgs().at("main").get();
  ControlFlowGraph *cfg12 = wpi12->cfgs().at("main").get();
  EXPECT_GE(cfg2->nodes().size(), cfg1->nodes().size());
  EXPECT_EQ(cfg2->nodes().size(), cfg12->nodes().size());

  std::set<std::pair<uint64_t, uint64_t>> edge_set1;
  std::set<std::pair<uint64_t, uint64_t>> edge_set2;
  std::set<std::pair<uint64_t, uint64_t>> edge_set12;
  auto initialize_edge_set =
      [](ControlFlowGraph *cfg,
         std::set<std::pair<uint64_t, uint64_t>> &edge_set) {
        for (auto &edge : cfg->intra_edges())
          edge_set.emplace(edge->src()->symbol_ordinal(),
                           edge->sink()->symbol_ordinal());
      };
  initialize_edge_set(cfg1, edge_set1);
  initialize_edge_set(cfg2, edge_set2);
  initialize_edge_set(cfg12, edge_set12);

  std::set<std::pair<uint64_t, uint64_t>> sym_diff;
  std::set_symmetric_difference(edge_set1.begin(), edge_set1.end(),
                                edge_set2.begin(), edge_set2.end(),
                                std::inserter(sym_diff, sym_diff.begin()));

  // Perfdata 1 & 2 contain different edge sets, this is because perfdata2 is
  // collected with an additional test run argument that directs the binary to
  // run in a different code path.  Refer to "propeller_sample_1.c"  block under
  // "if (argc > 1) {".

  // edges constructed from perf1 differs from those constructed from perf2.
  EXPECT_THAT(sym_diff, Not(IsEmpty()));

  std::set<std::pair<uint64_t, uint64_t>> union12;
  std::set_union(edge_set1.begin(), edge_set1.end(), edge_set2.begin(),
                 edge_set2.end(), std::inserter(union12, union12.begin()));
  // The union of edges constructed from perf1 and perf2 separately equals to
  // those constructed from perf1 and perf2 together.
  EXPECT_EQ(union12, edge_set12);

  auto accumutor = [](uint64_t acc,
                      const std::unique_ptr<CFGEdge> &e) -> uint64_t {
    return acc + e->weight();
  };
  uint64_t weight1 = std::accumulate(cfg1->intra_edges().begin(),
                                     cfg1->intra_edges().end(), 0, accumutor);
  uint64_t weight2 = std::accumulate(cfg2->intra_edges().begin(),
                                     cfg2->intra_edges().end(), 0, accumutor);
  uint64_t weight12 = std::accumulate(cfg12->intra_edges().begin(),
                                      cfg12->intra_edges().end(), 0, accumutor);
  // The sum of weights collected from perf file 1 & 2 separately equals to the
  // sum of weights collected from perf file 1 & 2 in oneshot.
  EXPECT_EQ(weight1 + weight2, weight12);
}

TEST(LlvmPropellerWholeProgramInfoBbInfoTest, DuplicateSymbolsDropped) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(
              GetAutoFdoTestDataFilePath("propeller_duplicate_symbols.bin")));
  std::unique_ptr<PropellerWholeProgramInfo> wpi =
        PropellerWholeProgramInfo::Create(options);
  ASSERT_NE(wpi.get(), nullptr);
  ASSERT_OK(wpi->ReadBinaryInfo());

  EXPECT_THAT(wpi->SelectFunctions(CfgCreationMode::kAllFunctions, nullptr),
              Not(IsEmpty()));
  // Multiple symbols have the "sample1_func1" name hence none of them will be
  // kept. Other functions are not affected.
  EXPECT_THAT(
      GetBBAddrMapByFunctionName(*wpi),
      AllOf(Contains(Pair("sample1_func", BbAddrMapIs(_, IsEmpty()))),
            Contains(Pair("compute_flag", BbAddrMapIs(_, Not(IsEmpty()))))));
  EXPECT_GE(wpi->stats().duplicate_symbols, 1);
}

TEST(LlvmPropellerWholeProgramInfoBbInfoTest, NoneDotTextSymbolsDropped) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(
              GetAutoFdoTestDataFilePath("propeller_sample_section.bin")));
  std::unique_ptr<PropellerWholeProgramInfo> wpi =
        PropellerWholeProgramInfo::Create(options);
  EXPECT_NE(wpi.get(), nullptr);
  ASSERT_OK(wpi->ReadBinaryInfo());

  EXPECT_THAT(wpi->SelectFunctions(CfgCreationMode::kAllFunctions, nullptr),
              Not(IsEmpty()));
  // "anycall" is inside ".anycall.anysection", so it should not be processed by
  // propeller. ".text.unlikely" function symbols are processed. Other functions
  // are not affected.
  EXPECT_THAT(
      GetBBAddrMapByFunctionName(*wpi),
      AllOf(Contains(Pair("anycall", BbAddrMapIs(_, IsEmpty()))),
            Contains(Key("unlikelycall")), Contains(Key("compute_flag"))));
}

TEST(LlvmPropellerWholeProgramInfo, DuplicateUniqNames) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(
              GetAutoFdoTestDataFilePath("duplicate_unique_names.out")));
  std::unique_ptr<PropellerWholeProgramInfo> wpi =
        PropellerWholeProgramInfo::Create(options);
  ASSERT_NE(wpi.get(), nullptr);
  ASSERT_OK(wpi->ReadBinaryInfo());

  EXPECT_THAT(wpi->SelectFunctions(CfgCreationMode::kAllFunctions, nullptr),
              Not(IsEmpty()));
  // We have 3 duplicated symbols, the last 2 are marked as duplicate_symbols.
  // 11: 0000000000001880     6 FUNC    LOCAL  DEFAULT   14
  //                     _ZL3foov.__uniq.148988607218547176184555965669372770545
  // 13: 00000000000018a0     6 FUNC    LOCAL  DEFAULT   1
  //                     _ZL3foov.__uniq.148988607218547176184555965669372770545
  // 15: 00000000000018f0     6 FUNC    LOCAL  DEFAULT   14
  //                     _ZL3foov.__uniq.148988607218547176184555965669372770545
  EXPECT_EQ(wpi->stats().duplicate_symbols, 2);
}

TEST(LlvmPropellerWholeProgramInfo, CreateCfgsOnlyForHotFunctions) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetKeepFrontendIntermediateData(true)  // need "symtab_" for testing.
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample_1.bin"))
          .AddPerfNames(
              GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1")));
  std::unique_ptr<PropellerWholeProgramInfo> wpi =
        PropellerWholeProgramInfo::Create(options);
  ASSERT_NE(wpi.get(), nullptr);
  ASSERT_OK(wpi->CreateCfgs(CfgCreationMode::kOnlyHotFunctions));

  const PropellerWholeProgramInfo::CFGMapTy &cfgs = wpi->cfgs();
  // "sample1_func" is cold, and should not have CFG.
  EXPECT_THAT(cfgs, Not(Contains(Pair("sample1_func", _))));
  // But "sample1_func" exists in symtab_.
  EXPECT_THAT(GetAllFunctionNamesFromSymtab(wpi->symtab()),
              Contains("sample1_func"));
  // "main" is hot function.
  EXPECT_THAT(cfgs, Contains(Pair("main", _)));
}

TEST(LlvmPropellerWholeProgramInfo, CheckNoHotFunctionsInSymtab) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetKeepFrontendIntermediateData(false)
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample_1.bin"))
          .AddPerfNames(
              GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1")));
  std::unique_ptr<PropellerWholeProgramInfo> wpi =
        PropellerWholeProgramInfo::Create(options);
  ASSERT_NE(wpi.get(), nullptr);
  auto pd = wpi->ParsePerfData();
  ASSERT_OK(pd);
  devtools_crosstool_autofdo::LBRAggregation lbr_aggregation = std::move(pd.value());
  ASSERT_OK(wpi->ReadBinaryInfo());

  wpi->DropNonSelectedFunctions(wpi->CalculateHotFunctions(lbr_aggregation));

  // "sample1_func" is cold, and should not exists in symtab.
  for (const auto &i : wpi->symtab())
    for (llvm::object::SymbolRef sym : i.second)
      EXPECT_NE(llvm::cantFail(sym.getName()), "sample1_func");
  // "main" is hot function, check it is in symtab.
  bool found_main = false;
  for (const auto &i : wpi->symtab())
    for (llvm::object::SymbolRef sym : i.second)
      found_main |= llvm::cantFail(sym.getName()) == "main";
  EXPECT_TRUE(found_main);
  EXPECT_THAT(GetBBAddrMapByFunctionName(*wpi),
              AllOf(Contains(Pair("main", BbAddrMapIs(_, Not(IsEmpty())))),
                    Contains(Pair("sample1_func", BbAddrMapIs(_, IsEmpty())))));
}

// Generates 2 cfg sets, one with "only_for_hot_functions" set to true, the
// other false and compare the two cfg sets.
TEST(LlvmPropellerWholeProgramInfo,
     OnlyGenerateCfgsForHotFunctionsOptimizationShouldNotChangeProfile) {
  const std::string binary_name =
      GetAutoFdoTestDataFilePath("propeller_clang_labels.binary");
  const std::string profile_name =
      GetAutoFdoTestDataFilePath("propeller_clang_labels.perfdata");
  const PropellerOptions options =
      PropellerOptions(PropellerOptionsBuilder()
                           .SetBinaryName(binary_name)
                           .AddPerfNames(profile_name)
                           .SetProfiledBinaryName("clang-12"));

  std::unique_ptr<PropellerWholeProgramInfo> wpi1 =
      PropellerWholeProgramInfo::Create(options);
  EXPECT_OK(wpi1->CreateCfgs(CfgCreationMode::kOnlyHotFunctions));
  std::vector<ControlFlowGraph *> hot_cfgs1 = wpi1->GetHotCfgs();

  std::unique_ptr<PropellerWholeProgramInfo> wpi2 =
      PropellerWholeProgramInfo::Create(options);
  EXPECT_OK(wpi2->CreateCfgs(CfgCreationMode::kOnlyHotFunctions));
  std::vector<ControlFlowGraph *> hot_cfgs2 = wpi2->GetHotCfgs();

  auto compare_node_ptr_attribtues = [](const CFGNode *n1, const CFGNode *n2) {
    return n1->size() == n2->size() && n1->addr() == n2->addr() &&
           n1->is_entry() == n2->is_entry() && n1->freq() == n2->freq();
  };

  auto compare_node_attribtues = [&compare_node_ptr_attribtues](
                                     const std::unique_ptr<CFGNode> &n1,
                                     const std::unique_ptr<CFGNode> &n2) {
    return compare_node_ptr_attribtues(n1.get(), n2.get());
  };

  auto compare_edge = [&compare_node_ptr_attribtues](const CFGEdge *e1,
                                                     const CFGEdge *e2) {
    return compare_node_ptr_attribtues(e1->src(), e2->src()) &&
           compare_node_ptr_attribtues(e1->sink(), e2->sink()) &&
           e1->weight() == e2->weight() && e1->IsCall() == e2->IsCall() &&
           e1->IsReturn() == e2->IsReturn() &&
           e1->IsFallthrough() == e2->IsFallthrough();
  };

  auto compare_edges = [&compare_edge](const std::vector<CFGEdge *> &v1,
                                       const std::vector<CFGEdge *> &v2) {
    if (v1.size() != v2.size()) return false;
    // Sort edges by src nodes's addresses and compare them one by one.
    auto edge_sorter = [](CFGEdge *e1, CFGEdge *e2) {
      return e1->src()->addr() < e2->src()->addr();
    };
    std::vector<CFGEdge *> u1 = v1;
    std::vector<CFGEdge *> u2(v2);
    std::sort(u1.begin(), u1.end(), edge_sorter);
    std::sort(u2.begin(), u2.end(), edge_sorter);
    return absl::c_equal(u1, u2, compare_edge);
  };

  auto compare_node = [&compare_node_attribtues, &compare_edges](
                          const std::unique_ptr<CFGNode> &n1,
                          const std::unique_ptr<CFGNode> &n2) {
    return compare_node_attribtues(n1, n2) &&
           compare_edges(n1->inter_ins(), n2->inter_ins()) &&
           compare_edges(n1->inter_outs(), n2->inter_outs()) &&
           compare_edges(n1->intra_ins(), n2->intra_ins()) &&
           compare_edges(n1->intra_outs(), n2->intra_outs());
  };

  auto compare_cfg = [&compare_node](ControlFlowGraph *g1,
                                     ControlFlowGraph *g2) {
    if (g1->nodes().size() != g2->nodes().size())
      return false;
    if (g1->GetEntryNode()->addr() != g2->GetEntryNode()->addr()) return false;
    // Nodes are already sorted via offset, compare them one by one.
    return absl::c_equal(g1->nodes(), g2->nodes(), compare_node);
  };

  EXPECT_EQ(hot_cfgs1.size(), hot_cfgs2.size());
  EXPECT_TRUE(absl::c_equal(hot_cfgs1, hot_cfgs2, compare_cfg));
}

TEST(LlvmPropellerWholeProgramInfo, FindBbHandleIndexUsingBinaryAddress) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .AddPerfNames(
              GetAutoFdoTestDataFilePath("propeller_clang_labels.perfdata"))
          .SetBinaryName(
              GetAutoFdoTestDataFilePath("propeller_clang_labels.binary"))
          .SetProfiledBinaryName("clang-12")
          .SetKeepFrontendIntermediateData(true));
  std::unique_ptr<PropellerWholeProgramInfo> wpi =
      PropellerWholeProgramInfo::Create(options);
  ASSERT_OK(wpi->ReadBinaryInfo());
  EXPECT_THAT(wpi->SelectFunctions(CfgCreationMode::kAllFunctions, nullptr),
              Not(IsEmpty()));
  // At address 0x000001b3d0a8, we have the following symbols all of size zero.
  //   BB.447 BB.448 BB.449 BB.450 BB.451 BB.452 BB.453 BB.454 BB.455
  //   BB.456 BB.457 BB.458 BB.459 BB.460

  auto bb_index_from_handle_index = [&](int index) {
    return wpi->bb_handles()[index].bb_index;
  };
  EXPECT_THAT(
      wpi->FindBbHandleIndexUsingBinaryAddress(0x1b3d0a8, BranchDirection::kTo),
      Optional(ResultOf(bb_index_from_handle_index, 447)));
  // At address 0x000001b3f5b0: we have the following symbols:
  //   Func<_ZN5clang18CompilerInvocation14CreateFromArgs...> BB.0 {size: 0x9a}
  EXPECT_THAT(
      wpi->FindBbHandleIndexUsingBinaryAddress(0x1b3f5b0, BranchDirection::kTo),
      Optional(ResultOf(bb_index_from_handle_index, 0)));
  // At address 0x1e63500: we have the following symbols:
  //   Func<_ZN4llvm22FoldingSetIteratorImplC2EPPv> BB.0 {size: 0}
  //                                                BB.1 {size: 0x8}
  EXPECT_THAT(
      wpi->FindBbHandleIndexUsingBinaryAddress(0x1e63500, BranchDirection::kTo),
      Optional(ResultOf(bb_index_from_handle_index, 0)));
  EXPECT_THAT(wpi->FindBbHandleIndexUsingBinaryAddress(0x1e63500,
                                                       BranchDirection::kFrom),
              Optional(ResultOf(bb_index_from_handle_index, 1)));
}
}  // namespace
