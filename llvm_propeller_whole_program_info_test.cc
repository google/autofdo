#include "llvm_propeller_whole_program_info.h"

#include <string>

#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_mock_whole_program_info.h"
#include "llvm_propeller_options.h"
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
using ::devtools_crosstool_autofdo::PropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::SymbolEntry;

static std::string GetAutoFdoTestDataFilePath(const std::string &filename) {
  const std::string testdata_filepath =
      FLAGS_test_srcdir +
      "/testdata/" + filename;
  return testdata_filepath;
}

TEST(LlvmPropellerWholeProgramInfo, CreateCFG) {
  const std::string binary = GetAutoFdoTestDataFilePath("propeller_sample.bin");
  const std::string perfdata =
      GetAutoFdoTestDataFilePath("propeller_sample.perfdata");

  const PropellerOptions options =  PropellerOptions()
          .WithBinaryName(binary)
          .WithPerfName(perfdata)
          .WithOutName("dummy.out")
          .WithProfiledBinaryName("propeller_sample.bin");

  auto wpi = PropellerWholeProgramInfo::Create(options);
  ASSERT_TRUE(wpi.get());

  ASSERT_TRUE(wpi->CreateCfgs());

  // Test resources are released after CreateCFG.
  EXPECT_TRUE(wpi->name_map().empty());
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
  auto options = PropellerOptions().WithPerfName(protobuf_input);
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
  auto wpi = PropellerWholeProgramInfo::Create(
      PropellerOptions().WithBinaryName(binary).WithOutName("dummy.out"));
  ASSERT_NE(nullptr, wpi) << "Could not initialize whole program info";
  EXPECT_TRUE(wpi->binary_has_bb_addr_map_section());
}

TEST(LlvmPropellerWholeProgramInfoBbAddrTest, BbAddrMapReadSymbolTable) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  auto wpi = PropellerWholeProgramInfo::Create(
      PropellerOptions().WithBinaryName(binary).WithOutName("dummy.out"));
  ASSERT_NE(nullptr, wpi) << "Could not initialize whole program info";
  ASSERT_TRUE(wpi->binary_has_bb_addr_map_section());
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

TEST(LlvmPropellerWholeProgramInfo, CalculateFallthroughs) {
  PropellerOptions options = PropellerOptions()
                                 .WithBinaryName(GetAutoFdoTestDataFilePath(
                                     "propeller_bblabels_aliases.bin"))
                                 .WithKeepFrontendIntermediateData(true);

  auto wpi = PropellerWholeProgramInfo::Create(options);
  ASSERT_NE(wpi.get(), nullptr);
  ASSERT_TRUE(wpi->PopulateSymbolMap());

  auto a1 = wpi->FindBasicblock("a.BB.main2");
  auto a2 = wpi->FindBasicblock("aa.BB.main2");
  auto a3 = wpi->FindBasicblock("aaa.BB.main2");
  auto a6 = wpi->FindBasicblock("aaaaaa.BB.main2");
  auto a7 = wpi->FindBasicblock("aaaaaaa.BB.main2");
  auto a10 = wpi->FindBasicblock("aaaaaaaaaa.BB.main2");

  ASSERT_NE(a1, nullptr);
  ASSERT_NE(a2, nullptr);
  ASSERT_NE(a3, nullptr);
  ASSERT_NE(a6, nullptr);
  ASSERT_NE(a7, nullptr);
  ASSERT_NE(a10, nullptr);
  std::vector<const SymbolEntry *> path;
  EXPECT_TRUE(wpi->CalculateFallthroughBBs(*a1, *a2, &path));
  // a.BB.foo and aa.BB.foo are adjacent. No other BBs in between.
  EXPECT_TRUE(path.empty());

  ASSERT_TRUE(wpi->CalculateFallthroughBBs(*a1, *a3, &path));
  EXPECT_EQ(path.size(), 1);
  // a2 is on the path a1->a3
  EXPECT_EQ(path.front(), a2);

  ASSERT_TRUE(wpi->CalculateFallthroughBBs(*a1, *a7, &path));
  EXPECT_EQ(path.size(), 6);
  EXPECT_EQ(a10->addr, a6->addr);
  EXPECT_NE(a10->ordinal, a6->ordinal);
  // a2, a3, a4, a5, r10, a6 is on the path a1->a3
  // Note r10 and a6 have same address, but both are included.
  EXPECT_EQ(path.front(), a2);
  EXPECT_EQ(path[1], a3);
  EXPECT_THAT(path, ::testing::Contains(a10));
  EXPECT_THAT(path, ::testing::Contains(a6));

  auto ar = wpi->FindBasicblock("raaaaaaa.BB.main2");
  auto a9 = wpi->FindBasicblock("aaaaaaaaa.BB.main2");
  ASSERT_NE(ar, nullptr);
  // raaaaaaa.BB.main2 and aaaaaaaaa.BB.main2 have same address, but they are 2
  // different symbols.
  EXPECT_EQ(ar->addr, a9->addr);
  EXPECT_NE(a9->ordinal, ar->ordinal);
  EXPECT_NE(ar, a9);
  ASSERT_TRUE(wpi->CalculateFallthroughBBs(*a7, *ar, &path));
  EXPECT_EQ(path.size(), 1);
  // a9 is on the path: a7 -> ar, although a9 and a7 share same address. We
  // include a9, but we exclude ar (because ar equals to "to").
  EXPECT_EQ(path.front(), a9);
}

TEST(LlvmPropellerWholeProgramInfoBbAddrMapTest, SkipEntryIfSymbolNotInSymtab) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_from_icu_genrb");
  auto wpi = PropellerWholeProgramInfo::Create(
      PropellerOptions().WithBinaryName(binary).WithOutName("dummy.out"));
  ASSERT_NE(nullptr, wpi) << "Could not initialize whole program info";
  ASSERT_TRUE(wpi->binary_has_bb_addr_map_section());
  wpi->ReadSymbolTable();
  ASSERT_TRUE(wpi->ReadBbAddrMapSection());
  EXPECT_GT(wpi->stats().bbaddrmap_function_does_not_have_symtab_entry, 1);
}

TEST(LlvmPropellerWholeProgramInfoBbAddrMapTest, ReadBbAddrMap) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  auto wpi = PropellerWholeProgramInfo::Create(
      PropellerOptions().WithBinaryName(binary).WithOutName("dummy.out"));
  ASSERT_NE(nullptr, wpi) << "Could not initialize whole program info";
  ASSERT_TRUE(wpi->binary_has_bb_addr_map_section());
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
  const PropellerOptions options =
      PropellerOptions()
          .WithBinaryName(
              GetAutoFdoTestDataFilePath("propeller_sample.bin"))
          .WithPerfName(GetAutoFdoTestDataFilePath(
              "propeller_sample.perfdata"))
          .WithOutName("dummy.out")
          .WithKeepFrontendIntermediateData(true)
          .WithProfiledBinaryName("propeller_sample.bin");
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
  const PropellerOptions options =
      PropellerOptions()
          .WithBinaryName(
              GetAutoFdoTestDataFilePath("propeller_sample.bin"))
          .WithPerfName(GetAutoFdoTestDataFilePath(
              "propeller_sample.perfdata"))
          .WithOutName("dummy.out")
          .WithKeepFrontendIntermediateData(true)
          .WithProfiledBinaryName("propeller_sample.bin");
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
  const PropellerOptions options =
      PropellerOptions()
          .WithBinaryName(
              GetAutoFdoTestDataFilePath("propeller_sample_O0.bin"))
          .WithPerfName(GetAutoFdoTestDataFilePath(
              "propeller_sample.perfdata"))
          .WithOutName("dummy.out")
          .WithKeepFrontendIntermediateData(true)
          .WithProfiledBinaryName("propeller_sample.bin");
  auto whole_program_info = PropellerWholeProgramInfo::Create(options);
  ASSERT_EQ(whole_program_info, nullptr);
}
}  // namespace
