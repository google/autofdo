#include "profile_symbol_list.h"

#include <memory>

#include "gmock/gmock.h"
#include "third_party/abseil/absl/memory/memory.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace devtools_crosstool_autofdo {
#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

TEST(ProfileSymbolListTest, EmptyNameList) {
  SymbolMap map;
  auto list = absl::make_unique<llvm::sampleprof::ProfileSymbolList>();
  fillProfileSymbolList(list.get(), /*name_size_list=*/{}, &map);
  EXPECT_EQ(list->size(), 0);
}

TEST(ProfileSymbolListTest, Threshold0) {
  SymbolMap map;
  auto list = absl::make_unique<llvm::sampleprof::ProfileSymbolList>();
  fillProfileSymbolList(list.get(), /*name_size_list=*/{{"foo", 0}}, &map,
                        /*size_threshold_frac=*/0);
  EXPECT_EQ(list->size(), 0);
}

TEST(ProfileSymbolListTest, Threshold100) {
  SymbolMap map;
  auto list = absl::make_unique<llvm::sampleprof::ProfileSymbolList>();
  fillProfileSymbolList(list.get(),
                        /*name_size_list=*/{{"foo", 10}}, &map,
                        /*size_threshold_frac=*/1.0);
  EXPECT_EQ(list->size(), 1);
}

TEST(ProfileSymbolListTest, Threshold100Size0) {
  SymbolMap map;
  auto list = absl::make_unique<llvm::sampleprof::ProfileSymbolList>();
  fillProfileSymbolList(
      list.get(),
      /*name_size_list=*/{{"foo", 10}, {"bar", 20}, {"f", 0}, {"g", 0}}, &map,
      /*size_threshold_frac=*/1.0);
  // Size 0 symbols are always excluded.
  EXPECT_EQ(list->size(), 2);
}

TEST(ProfileSymbolListTest, Threshold001Size0) {
  SymbolMap map;
  auto list = absl::make_unique<llvm::sampleprof::ProfileSymbolList>();
  fillProfileSymbolList(
      list.get(),
      /*name_size_list=*/{{"foo", 10}, {"bar", 20}, {"f", 0}, {"g", 0}}, &map,
      /*size_threshold_frac=*/0.01);
  EXPECT_EQ(list->size(), 0);
}

}  // namespace
}  // namespace devtools_crosstool_autofdo
