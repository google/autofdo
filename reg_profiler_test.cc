#include <iostream>
#include <string>
#include <memory>

#include "reg_profiler.h"
#include "gtest/gtest.h"
#include "llvm_propeller_whole_program_info.h"
#include "llvm_propeller_options_builder.h"

#include "llvm/MC/MCInst.h"

using devtools_crosstool_autofdo::PropellerOptions;
using devtools_crosstool_autofdo::PropellerOptionsBuilder;
using devtools_crosstool_autofdo::PropellerWholeProgramInfo;
using devtools_crosstool_autofdo::PushPopCounter;

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace devtools_crosstool_autofdo {

/** get the example assembly by
 * objdump --disassemble=test blaze-bin/experimental/users/xiaofans/reg_profiler/example
 */
TEST(Disassembler, base) {
  const std::string path = FLAGS_test_srcdir + "/testdata/reg_profiler_test.bin";
  printf("exe path = %s\n", path.c_str());
  auto owning_binary = llvm::object::createBinary(path);
  if (!owning_binary) ASSERT_TRUE(false);
  EXPECT_EQ(owning_binary->getBinary()->isELF(), true);

  auto* obj = llvm::cast<llvm::object::ELFObjectFileBase>(
      owning_binary->getBinary());
  PushPopCounter::Disassembler disassembler(obj);
  llvm::object::SectionRef out_section;

  int i = 0;
  std::string t[50];

  bool success = disassembler.DisassembleRange(
    0x1150, 0x11a5-0x1150, out_section,
      [&](llvm::MCInst inst, PushPopCounter::Disassembler::Context &ctx) -> bool {
        auto name = out_section.getName();
        EXPECT_TRUE(static_cast<bool>(name));
        if (name) EXPECT_TRUE(name->startswith(".text"));
        EXPECT_LT(i, 50);
        t[i++] = ctx.mii->getName(inst.getOpcode()).str();
        return false;
      }
  );
  EXPECT_TRUE(success);

  std::string opnames[] = {
    "PUSH64r",
    "MOV64rr",
    "SUB64ri8",
    "MOV32mr",
    "MOV32mi",
    "MOV32mi",
    "MOV32rm",
    "CMP32rm",
    "JCC_4",
    "MOV32rm",
    "ADD32rm",
    "MOV32mr",
    "MOV32rm",
    "ADD32ri8",
    "MOV32mr",
    "JMP_4",
    "MOV32rm",
    "LEA64r",
    "MOV8ri",
    "CALL64pcrel32",
    "XOR32rr",
    "ADD64ri8",
    "POP64r",
    "RET64"
  };

  for (int i = 0; i < 24; ++i) {
    EXPECT_EQ(t[i], opnames[i]);
  }

}




}  // namespace xiaofans::reg_profiler
