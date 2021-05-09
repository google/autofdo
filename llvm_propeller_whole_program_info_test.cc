#include "llvm_propeller_whole_program_info.h"

#include <algorithm>
#include <memory>
#include <string>

#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_mock_whole_program_info.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "llvm/Support/Path.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

using ::devtools_crosstool_autofdo::CFGEdge;
using ::devtools_crosstool_autofdo::CFGNode;
using ::devtools_crosstool_autofdo::ControlFlowGraph;
using ::devtools_crosstool_autofdo::MockPropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::PropellerOptions;
using ::devtools_crosstool_autofdo::PropellerOptionsBuilder;
using ::devtools_crosstool_autofdo::PropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::SymbolEntry;

static std::string GetAutoFdoTestDataFilePath(const std::string &filename) {
  const std::string testdata_filepath =
      FLAGS_test_srcdir +
      "/testdata/" +
      filename;
  return testdata_filepath;
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

  auto wpi = PropellerWholeProgramInfo::Create(options);
  ASSERT_TRUE(wpi.get());

  ASSERT_TRUE(wpi->CreateCfgs());

  // Test resources are released after CreateCFG.
  EXPECT_TRUE(wpi->binary_mmaps().empty());

  const ControlFlowGraph *main = wpi->FindCfg("main");
  EXPECT_TRUE(main);
  const ControlFlowGraph *compute_flag = wpi->FindCfg("compute_flag");
  EXPECT_TRUE(compute_flag);
  const ControlFlowGraph *sample1_func = wpi->FindCfg("sample1_func");
  EXPECT_TRUE(sample1_func);
  const ControlFlowGraph *this_is_vey_code = wpi->FindCfg("this_is_very_code");
  EXPECT_TRUE(this_is_vey_code);

  // Examine main nodes / edges.
  EXPECT_GE(main->nodes_.size(), 4);

  EXPECT_GE(main->inter_edges_.size(), 1);
  EXPECT_TRUE(main->inter_edges_.front()->IsCall());
  EXPECT_GT(main->inter_edges_.front()->weight_, 100);

  EXPECT_GE(compute_flag->inter_edges_.size(), 1);
  EXPECT_TRUE(compute_flag->inter_edges_.front()->IsReturn());
  EXPECT_GT(compute_flag->inter_edges_.front()->weight_, 100);
}

// This test checks that the mock can load a CFG from the serialized format
// correctly.
TEST(LlvmPropellerMockWholeProgramInfo, CreateCFGFromProto) {
  const std::string protobuf_input =
      GetAutoFdoTestDataFilePath("propeller_sample.protobuf");
  const PropellerOptions options(
      PropellerOptionsBuilder().AddPerfNames(protobuf_input));
  MockPropellerWholeProgramInfo wpi(options);
  ASSERT_TRUE(wpi.CreateCfgs());

  EXPECT_GT(wpi.cfgs().size(), 10);

  // Check some inter-func edge is valid.
  const ControlFlowGraph *main = wpi.FindCfg("main");
  EXPECT_NE(main, (nullptr));
  EXPECT_FALSE(main->inter_edges_.empty());
  CFGEdge &edge = *(main->inter_edges_.front());
  EXPECT_EQ(edge.src_->cfg_, main);
  EXPECT_NE(edge.sink_->cfg_, main);
  // The same "edge" instance exists both in src->inter_outs_ and
  // sink->inter_ins_.
  EXPECT_NE(std::find(edge.sink_->inter_ins_.begin(),
                      edge.sink_->inter_ins_.end(), &edge),
            edge.sink_->inter_ins_.end());
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
  wpi->ReadSymbolTable();
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
  wpi->ReadSymbolTable();
  ASSERT_TRUE(wpi->ReadBbAddrMapSection());
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
  wpi->ReadSymbolTable();
  ASSERT_TRUE(wpi->ReadBbAddrMapSection());
  EXPECT_GT(wpi->bb_addr_map().at("compute_flag").size(), 0);
  SymbolEntry *fsym = wpi->bb_addr_map().at("compute_flag").front()->func_ptr;
  ASSERT_NE(nullptr, fsym);
  EXPECT_TRUE(fsym->IsFunction());
  EXPECT_EQ(fsym->ordinal + 1,
            wpi->bb_addr_map().at("compute_flag")[0]->ordinal);
  uint64_t last_ordinal = fsym->ordinal;
  for (auto *sym : wpi->bb_addr_map().at("compute_flag")) {
    EXPECT_TRUE(sym->IsBasicBlock());
    EXPECT_EQ(sym->func_ptr, fsym);
    EXPECT_EQ(last_ordinal, sym->ordinal - 1);
    last_ordinal = sym->ordinal;
  }
  // The last bb of compute_flag_symbol function is a return block.
  EXPECT_TRUE(wpi->bb_addr_map().at("compute_flag").back()->IsReturnBlock());

  // Check ordinals are monotonically increasing.
  uint64_t prev_ordinal = 0;
  for (auto &l1 : wpi->address_map()) {
    for (const std::unique_ptr<SymbolEntry> &sym : l1.second) {
      EXPECT_LT(prev_ordinal, sym->ordinal);
      prev_ordinal = sym->ordinal;
    }
  }
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
  ASSERT_TRUE(whole_program_info->CreateCfgs());
  auto &cfgs = whole_program_info->cfgs();
  auto ii = cfgs.find("main");
  EXPECT_TRUE(ii != cfgs.end());
  const ControlFlowGraph &main_cfg = *(ii->second);
  EXPECT_GT(main_cfg.nodes_.size(), 1);
  CFGNode *entry = main_cfg.nodes_.begin()->get();
  SymbolEntry *main_sym =
      whole_program_info->bb_addr_map().at("main").front()->func_ptr;
  // "main_sym" is a function symbol, and function symbol does not correspond to
  // any CFGNode. The first CFGNode of a ControlFlowGraph is the entry node,
  // which represents the first basic block symbol of a functon symbol.
  EXPECT_EQ(main_sym->ordinal, entry->symbol_ordinal_ - 1);
  EXPECT_EQ(main_sym->func_ptr, main_sym);
  // Function's size > entry bb size.
  EXPECT_GT(main_sym->size, entry->size_);
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
  ASSERT_TRUE(whole_program_info->CreateCfgs());
  EXPECT_GT(whole_program_info->stats().cfgs_created, 3);
  EXPECT_GT(whole_program_info->stats().edges_created, 3);
  EXPECT_GT(whole_program_info->stats().nodes_created, 8);
  EXPECT_GT(whole_program_info->stats().syms_created, 10);
  EXPECT_GT(whole_program_info->stats().binary_mmap_num, 1);
  EXPECT_EQ(whole_program_info->stats().dropped_symbols, 0);
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
    EXPECT_TRUE(wpi->CreateCfgs());
    return wpi;
  };

  std::string perf1 =
      GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1");
  std::string perf2 =
      GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata2");
  auto wpi1 = init_whole_program_info(std::vector<std::string>{perf1});
  auto wpi2 = init_whole_program_info(std::vector<std::string>{perf2});
  auto wpi12 = init_whole_program_info(std::vector<std::string>{perf1, perf2});

  ControlFlowGraph *cfg1 = wpi1->cfgs().at("main").get();
  ControlFlowGraph *cfg2 = wpi2->cfgs().at("main").get();
  ControlFlowGraph *cfg12 = wpi12->cfgs().at("main").get();
  EXPECT_EQ(cfg1->nodes_.size(), cfg2->nodes_.size());
  EXPECT_EQ(cfg1->nodes_.size(), cfg12->nodes_.size());

  std::set<std::pair<uint64_t, uint64_t>> edge_set1;
  std::set<std::pair<uint64_t, uint64_t>> edge_set2;
  std::set<std::pair<uint64_t, uint64_t>> edge_set12;
  auto initialize_edge_set =
      [](ControlFlowGraph *cfg,
         std::set<std::pair<uint64_t, uint64_t>> &edge_set) {
        for (auto &edge : cfg->intra_edges_)
          edge_set.emplace(edge->src_->symbol_ordinal_,
                           edge->sink_->symbol_ordinal_);
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
  EXPECT_FALSE(sym_diff.empty());

  std::set<std::pair<uint64_t, uint64_t>> union12;
  std::set_union(edge_set1.begin(), edge_set1.end(), edge_set2.begin(),
                 edge_set2.end(), std::inserter(union12, union12.begin()));
  // The union of edges constructed from perf1 and perf2 separately equals to
  // those constructed from perf1 and perf2 together.
  EXPECT_EQ(union12, edge_set12);

  auto accumutor = [](uint64_t acc, std::unique_ptr<CFGEdge> &e) -> uint64_t {
    return acc + e->weight_;
  };
  uint64_t weight1 = std::accumulate(cfg1->intra_edges_.begin(),
                                     cfg1->intra_edges_.end(), 0, accumutor);
  uint64_t weight2 = std::accumulate(cfg2->intra_edges_.begin(),
                                     cfg2->intra_edges_.end(), 0, accumutor);
  uint64_t weight12 = std::accumulate(cfg12->intra_edges_.begin(),
                                      cfg12->intra_edges_.end(), 0, accumutor);
  // The sum of weights collected from perf file 1 & 2 separately equals to the
  // sum of weights collected from perf file 1 & 2 in oneshot.
  EXPECT_EQ(weight1 + weight2, weight12);
}

TEST(LlvmPropellerWholeProgramInfoBbInfoTest, TestDroppingInvalidDataFile) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(GetAutoFdoTestDataFilePath("propeller_sample_1.bin"))
          .AddPerfNames(
              GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata1"))
          .AddPerfNames(
              GetAutoFdoTestDataFilePath("propeller_sample_1.perfdata")));
  std::unique_ptr<PropellerWholeProgramInfo> wpi =
      PropellerWholeProgramInfo::Create(options);
  ASSERT_NE(wpi.get(), nullptr);
  EXPECT_TRUE(wpi->CreateCfgs());
  // "propeller_sample.perfdata" is not a valid perfdata file for
  // "propeller_sample_1.bin", so wpi only processes 1 profile.
  EXPECT_EQ(wpi->stats().perf_file_parsed, 1);
}

TEST(LlvmPropellerWholeProgramInfoBbInfoTest, DuplicateSymbolsDropped) {
  const PropellerOptions options = PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(
              GetAutoFdoTestDataFilePath("propeller_duplicate_symbols.bin")));
  std::unique_ptr<PropellerWholeProgramInfo> wpi =
        PropellerWholeProgramInfo::Create(options);
    EXPECT_NE(wpi.get(), nullptr);

  EXPECT_TRUE(wpi->PopulateSymbolMap());
  const PropellerWholeProgramInfo::BbAddrMapTy &bb_addr_map =
      wpi->bb_addr_map();
  // Not a single instance of function that have duplicate names are kept.
  EXPECT_EQ(bb_addr_map.find("sample1_func"), bb_addr_map.end());
  // Other functions are not affected.
  EXPECT_NE(bb_addr_map.find("compute_flag"), bb_addr_map.end());
  EXPECT_GE(wpi->stats().duplicate_symbols, 1);
}
}  // namespace
