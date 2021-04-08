#include "llvm_propeller_profile_writer.h"

#include <memory>
#include <sstream>
#include <string>

#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_whole_program_info.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/strings/str_cat.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

using ::devtools_crosstool_autofdo::PropellerOptions;
using ::devtools_crosstool_autofdo::PropellerOptionsBuilder;
using ::devtools_crosstool_autofdo::PropellerProfWriter;
using ::devtools_crosstool_autofdo::PropellerWholeProgramInfo;
using ::devtools_crosstool_autofdo::SymbolEntry;

TEST(LlvmPropellerProfileWriterTest, FindBinaryBuildId) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "llvm_function_samples.binary");
  std::unique_ptr<PropellerWholeProgramInfo> pwi =
      PropellerWholeProgramInfo::Create(
          PropellerOptions(PropellerOptionsBuilder().SetBinaryName(binary)));
  ASSERT_TRUE(pwi);
  EXPECT_FALSE(pwi->binary_is_pie());
  EXPECT_STREQ(pwi->binary_build_id().c_str(),
               "a56e4274b3adf7c87d165ca6deb66db002e72e1e");
}

TEST(LlvmPropellerProfileWriterTest, PieAndNoBuildId) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_barebone_pie_nobuildid.bin");
  std::unique_ptr<PropellerWholeProgramInfo> pwi =
      PropellerWholeProgramInfo::Create(
          PropellerOptions(PropellerOptionsBuilder().SetBinaryName(binary)));
  ASSERT_TRUE(pwi);
  EXPECT_TRUE(pwi->binary_is_pie());
  EXPECT_TRUE(pwi->binary_build_id().empty());
}

TEST(LlvmPropellerProfileWriterTest, BBLabels1) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_bblabels.bin");
  std::unique_ptr<PropellerWholeProgramInfo> pwi =
      PropellerWholeProgramInfo::Create(
          PropellerOptions(PropellerOptionsBuilder()
                               .SetBinaryName(binary)
                               .SetKeepFrontendIntermediateData(true)));
  ASSERT_TRUE(pwi);
  EXPECT_TRUE(pwi->PopulateSymbolMap());
  const auto &name_map = pwi->name_map();
  auto si0 = name_map.find("main");
  EXPECT_NE(si0, name_map.end());

  const auto &main_map = si0->second;
  EXPECT_NE(main_map.find("aa"), main_map.end());
  EXPECT_NE(main_map.find("aaaaa"), main_map.end());
  EXPECT_NE(main_map.find(""), main_map.end());

  const SymbolEntry *mainSym = pwi->FindFunction("main");
  EXPECT_EQ(mainSym->func_ptr, mainSym);
  EXPECT_FALSE(mainSym->IsBasicBlock());
  EXPECT_STREQ(mainSym->name.str().c_str(), "main");
  EXPECT_EQ(mainSym->aliases.size(), 1);
  EXPECT_STREQ(mainSym->aliases[0].str().c_str(), "main");

  const SymbolEntry *main_sym = pwi->FindBasicblock("a.BB.main");
  EXPECT_TRUE(main_sym->IsBasicBlock());
  EXPECT_EQ(main_sym->func_ptr, mainSym);
  EXPECT_STREQ(main_sym->name.str().c_str(), "a");
}

TEST(LlvmPropellerProfileWriterTest, BBLabelsAliases) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_bblabels_aliases.bin");
  std::unique_ptr<PropellerWholeProgramInfo> pwi =
      PropellerWholeProgramInfo::Create(
          PropellerOptions(PropellerOptionsBuilder()
                               .SetBinaryName(binary)
                               .SetKeepFrontendIntermediateData(true)));
  ASSERT_TRUE(pwi);

  EXPECT_TRUE(pwi->PopulateSymbolMap());
  const SymbolEntry *main = pwi->FindFunction("main");
  const SymbolEntry *main2 = pwi->FindFunction("main2");
  const SymbolEntry *bMain = pwi->FindBasicblock("a.BB.main2");
  EXPECT_NE(main, (nullptr));
  EXPECT_NE(main2, (nullptr));
  EXPECT_NE(bMain, (nullptr));

  // main and main2 are aliases
  EXPECT_EQ(main, main2);

  // Primary name must be "main2", not main, because all bbs are grouped into
  // "main2", not "main".
  EXPECT_STREQ("main2", main->name.str().c_str());

  // Primary name must equal to alias[0].
  EXPECT_STREQ(main->aliases[0].str().c_str(), main->name.str().c_str());
  EXPECT_STREQ(main->aliases[1].str().c_str(), "main");
  EXPECT_EQ(2, main->aliases.size());

  const SymbolEntry *b1 = pwi->FindBasicblock("raaaaaaa.BB.main2");
  const SymbolEntry *b2 = pwi->FindBasicblock("aaaaaaaaa.BB.main2");
  // b1 & b2 are at the same address, but they must not alias.
  EXPECT_NE(b1, b2);

  // googse and empty_main_bb are at the same address.
  // googse is a non-empty function, empty_main_bb is an empty bb, grouped into
  // main2.
  const SymbolEntry *goose = pwi->FindFunction("goose");
  const SymbolEntry *empty_main_bb = pwi->FindBasicblock("rr.BB.main2");
  // empty_main_bb and goose have the same address and back-to-back ordinals.
  EXPECT_EQ(goose->addr, empty_main_bb->addr);
  EXPECT_EQ(empty_main_bb->ordinal + 1, goose->ordinal);
  // empty_main_bb is layouted BEFORE goose.
  EXPECT_LT(empty_main_bb->ordinal, goose->ordinal);
  // And empty_main_bb is grouped into main, not goose.
  EXPECT_EQ(empty_main_bb->func_ptr, pwi->FindFunction("main"));

  // Now a.BB.goose2 and goose2 are at the same address.
  // a.BB.goose2 must group into goose2
  const SymbolEntry *goose2 = pwi->FindFunction("goose2");
  const SymbolEntry *a_bb_goose2 = pwi->FindBasicblock("a.BB.goose2");
  EXPECT_EQ(a_bb_goose2->addr, goose2->addr);
  EXPECT_GT(a_bb_goose2->ordinal, goose2->ordinal);
  EXPECT_EQ(a_bb_goose2->func_ptr, goose2);
}

TEST(LlvmPropellerProfileWriterTest, ParsePerf0_relative_path) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  // Test that specifying --mmap_name with relative name works.
  auto writer_ptr = PropellerProfWriter::Create(PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(binary)
          .AddPerfNames(perfdata)
          .SetClusterOutName("dummy.out")
          .SetProfiledBinaryName("any_relative_path/propeller_sample.bin")));
  // When relative file path is passed to "--binary", we use the name portion of
  // the path (like `basename <filename>`) to match mmap entries. Since
  // propeller_sample.perfdata contains mmaps with file name
  // "/usr/local/google/home/shenhan/copt/llvm-propeller-2/plo/propeller_sample.bin",
  // writer_ptr must not be null.
  ASSERT_NE(nullptr, writer_ptr);
}

TEST(LlvmPropellerProfileWriterTest, ParsePerf0_absolute_path) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  auto writer_ptr = PropellerProfWriter::Create(PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(binary)
          .AddPerfNames(perfdata)
          .SetClusterOutName("dummy.out")
          .SetProfiledBinaryName("/any_absolute_path/propeller_sample.bin")));
  // When absolute file path is passed to "--binary", we use absolute path to
  // match mmap entries. Since propeller_sample.perfdata only contains mmap with
  // file name
  // "/usr/local/google/home/shenhan/copt/llvm-propeller-2/plo/propeller_sample.bin",
  // this is expected to fail.
  ASSERT_EQ(nullptr, writer_ptr);
}

TEST(LlvmPropellerProfileWriterTest, ParsePerf1) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  auto writer_ptr = PropellerProfWriter::Create(
      PropellerOptions(PropellerOptionsBuilder()
                           .SetBinaryName(binary)
                           .AddPerfNames(perfdata)
                           .SetClusterOutName("dummy.out")
                           .SetProfiledBinaryName("propeller_sample.bin")
                           .SetKeepFrontendIntermediateData(true)));
  ASSERT_NE(nullptr, writer_ptr);
  const PropellerWholeProgramInfo *pwi =
      static_cast<const PropellerWholeProgramInfo *>(
          writer_ptr->whole_program_info());
  // We started 3 instances, so mmap must be >= 3.
  EXPECT_GE(pwi->binary_mmaps().size(), 3);

  // All instances of the same pie binary cannot be loaded into same place.
  bool all_equals = true;
  for (auto i = pwi->binary_mmaps().begin(),
            e = std::prev(pwi->binary_mmaps().end());
       i != e; ++i)
    all_equals &= (i->second == std::next(i)->second);
  EXPECT_FALSE(all_equals);
}

TEST(LlvmPropellerProfileWriterTest, BinaryWithDuplicateSymbols) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_duplicate_symbols.bin");
  std::unique_ptr<PropellerWholeProgramInfo> pwi =
      PropellerWholeProgramInfo::Create(
          PropellerOptions(PropellerOptionsBuilder()
                           .SetBinaryName(binary)
                           .SetKeepFrontendIntermediateData(true)));
  ASSERT_TRUE(pwi);
  EXPECT_TRUE(pwi->PopulateSymbolMap());

  auto &nm = pwi->name_map();
  auto i = nm.find("sample1_func");
  EXPECT_EQ(i, nm.end());
  EXPECT_EQ(pwi->FindCfg("sample1_func"), nullptr);
  EXPECT_GT(pwi->stats().duplicate_symbols, 0);
}
}  // namespace
