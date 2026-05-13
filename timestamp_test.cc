#include <string>
#include "profile_creator.h"
#include "symbol_map.h"
#include "gmock/gmock.h"

using namespace devtools_crosstool_autofdo;

TEST(TimestampTest, AssignTimestampToSymbol) {
  std::string binary = ::testing::SrcDir() + "/testdata/llvm_function_samples.binary";
  std::string profile_name = ::testing::SrcDir() + "/testdata/llvm_function_samples_perf.data";
  std::string profiler = "perf";

  ProfileCreator creator(binary);
  SymbolMap symbol_map(binary);

  EXPECT_TRUE(creator.ReadSample(profile_name, profiler));
  symbol_map.ReadLoadableExecSegmentInfo(creator.IsKernelSample());
  EXPECT_TRUE(creator.ComputeProfile(&symbol_map, false));

  // The timestamp associated with function is the timestamp of first execution
  // of the first sampled instruction in the function.

  const Symbol *main_sym = symbol_map.GetSymbolByName("main");
  EXPECT_EQ(main_sym->timestamp, 801840661247321);

  const Symbol *Z3bari = symbol_map.GetSymbolByName("_Z3bari");
  EXPECT_EQ(Z3bari->timestamp, 801840748463085);

  const Symbol *Z3fooi = symbol_map.GetSymbolByName("_Z3fooi");
  EXPECT_EQ(Z3fooi->timestamp, 801841064917429);
}
