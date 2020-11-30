#include "llvm_profile_writer.h"
#include "addr2line.h"
#include "profile_creator.h"
#include "symbol_map.h"
#include "gmock/gmock.h"
#include "third_party/abseil/absl/strings/str_cat.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace devtools_crosstool_autofdo {

TEST(LlvmProfileWriterTest, ReadProfile) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "llvm_function_samples.binary");
  const std::string profile =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "llvm_function_samples_perf.data");

  // Read the profile for the binary.
  devtools_crosstool_autofdo::ProfileCreator creator(binary);
  ASSERT_TRUE(creator.ReadSample(profile, "perf"));

  SymbolMap symbol_map(binary);
  ASSERT_TRUE(creator.ComputeProfile(&symbol_map));

  StringIndexMap name_table;
  StringTableUpdater::Update(symbol_map, &name_table);

  LLVMProfileBuilder builder(name_table);
  const auto &profiles = builder.ConvertProfiles(symbol_map);

  // The file should have 3 functions: main, _Z3fooi and _Z3bari.
  ASSERT_EQ(profiles.size(), 3);

  // main's profile looks like this:
  //
  // main:1186160:0
  //  0: 0
  //  3.1: 1
  //  3.3: 1
  //  4.1: 2855
  //  4.3: 2855
  //  5: 5041
  //  5.1: 5041
  //  6: 9979
  //  7: 9890
  //  9: 10596 _Z3fooi:3336 _Z3bari:8045
  //  10: 2855
  //  12: 0
  //  14: 0
  //  10: _Z3bari:137340
  //   1: 9890
  const auto &main_profile_it = profiles.find("main");
  ASSERT_NE(main_profile_it, profiles.end());
  const auto &main_profile = main_profile_it->second;
  ASSERT_EQ(main_profile.getTotalSamples(), 1186160);
  ASSERT_EQ(main_profile.getBodySamples().size(), 13);
  ASSERT_EQ(*main_profile.findSamplesAt(4, 3), 2855);
  const llvm::sampleprof::LineLocation loc(9, 0);

  const auto &indirect_call_record = main_profile.getBodySamples().at(loc);
  ASSERT_TRUE(indirect_call_record.hasCalls());
  ASSERT_EQ(indirect_call_record.getSamples(), 10596);

  const auto& call_targets = indirect_call_record.getCallTargets();
  ASSERT_EQ(call_targets.size(), 2);
  ASSERT_EQ(call_targets.lookup("_Z3fooi"), 3336);
  ASSERT_EQ(call_targets.lookup("_Z3bari"), 8045);
}

TEST(LlvmProfileWriterTest, ConvertProfile) {
  SymbolMap symbol_map;
  symbol_map.set_count_threshold(1);
  symbol_map.AddSymbol("foo");
  SourceStack src1, src2;
  src1.push_back(SourceInfo("baz", "", "", 0, 200, 0));
  src1.push_back(SourceInfo("bar1", "", "", 0, 20, 0));
  src1.push_back(SourceInfo("foo", "", "", 0, 2, 0));
  symbol_map.AddSourceCount("foo", src1, 100, 1);
  src2.push_back(SourceInfo("bar2", "", "", 0, 30, 0));
  src2.push_back(SourceInfo("foo", "", "", 0, 3, 0));
  symbol_map.AddSourceCount("foo", src2, 200, 1);

  StringIndexMap name_table;
  StringTableUpdater::Update(symbol_map, &name_table);
  LLVMProfileBuilder builder(name_table);
  const auto &profiles = builder.ConvertProfiles(symbol_map);
  const auto &foo_profile_it = profiles.find("foo");
  ASSERT_NE(foo_profile_it, profiles.end());
#if LLVM_VERSION_MAJOR>=12
  const auto &bar1_profile = foo_profile_it->second.findFunctionSamplesAt(
      llvm::sampleprof::LineLocation(2, 0), "bar1", nullptr);
#else
  const auto &bar1_profile = foo_profile_it->second.findFunctionSamplesAt(
      llvm::sampleprof::LineLocation(2, 0), "bar1");
#endif
  CHECK(bar1_profile != nullptr);
#if LLVM_VERSION_MAJOR>=12
  const auto &bar2_profile = foo_profile_it->second.findFunctionSamplesAt(
      llvm::sampleprof::LineLocation(3, 0), "bar2", nullptr);
#else
  const auto &bar2_profile = foo_profile_it->second.findFunctionSamplesAt(
      llvm::sampleprof::LineLocation(3, 0), "bar2");
#endif
  CHECK_EQ(*bar2_profile->findSamplesAt(30, 0), 200);
  CHECK(bar2_profile != nullptr);
#if LLVM_VERSION_MAJOR>=12
  const auto &baz_profile = bar1_profile->findFunctionSamplesAt(
      llvm::sampleprof::LineLocation(20, 0), "baz", nullptr);
#else
  const auto &baz_profile = bar1_profile->findFunctionSamplesAt(
      llvm::sampleprof::LineLocation(20, 0), "baz");
#endif
  CHECK(baz_profile != nullptr);
  CHECK_EQ(*baz_profile->findSamplesAt(200, 0), 100);
}
}  // namespace devtools_crosstool_autofdo
