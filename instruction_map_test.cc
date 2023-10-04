// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// These tests check to see that instruction_map can read correct information
// from the binary.

#include "instruction_map.h"

#include <string>

#include "addr2line.h"
#include "sample_reader.h"
#include "symbol_map.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"

ABSL_FLAG(std::string, binary, "", "Binary file name");

using devtools_crosstool_autofdo::Addr2line;

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

class InstructionMapTest : public testing::Test {
 protected:
  static const char kTestDataDir[];

  InstructionMapTest() {}
};

const char InstructionMapTest::kTestDataDir[] =
    "/testdata/";

TEST_F(InstructionMapTest, PerFunctionInstructionMap) {
  Addr2line *addr2line = Addr2line::Create(FLAGS_test_srcdir +
                                           kTestDataDir + "test.binary");
  devtools_crosstool_autofdo::SymbolMap symbol_map(
      FLAGS_test_srcdir + kTestDataDir + "test.binary");
  devtools_crosstool_autofdo::PerfDataSampleReader sample_reader(
      FLAGS_test_srcdir + kTestDataDir + "test.lbr",
      "test.binary", "");
  ASSERT_TRUE(sample_reader.ReadAndSetTotalCount());
  devtools_crosstool_autofdo::InstructionMap inst_map(addr2line, &symbol_map);
  symbol_map.AddSymbol("longest_match");
  inst_map.BuildPerFunctionInstructionMap("longest_match", 0x401680, 0x401871);
  delete addr2line;
}
}  // namespace
