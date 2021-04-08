#include "disassembler.h"

#include "base/commandlineflags.h"
#include "gtest/gtest.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {
class DisassemblerTest : public testing::Test {
};

class TestDisassembler : public devtools_crosstool_autofdo::Disassembler {
 public:
  std::map<uint64, uint64> call_target_map_;
  std::map<uint64, uint64> cjmp_target_map_;
  std::map<uint64, uint64> djmp_target_map_;
  std::map<uint64, std::vector<uint64> > idjmp_targets_map_;
  std::vector<uint64> terminators_;

  std::set<uint64> GetAddrs() {
    return addr_set_;
  }

 protected:
  void HandleConditionalJump(uint64 addr,
                             uint64 fall_through,
                             uint64 target) override {
    cjmp_target_map_[addr] = target;
  }

  void HandleUnconditionalJump(uint64 addr, uint64 target) override {
    djmp_target_map_[addr] = target;
  }

  void HandleDirectCall(uint64 addr, uint64 target) override {
    call_target_map_[addr] = target;
  }

  void HandleIndirectJump(uint64 addr, std::vector<uint64> targets) override {
    idjmp_targets_map_[addr] = targets;
  }

  void HandleTerminator(uint64 addr) override {
    terminators_.push_back(addr);
  }
};

TEST_F(DisassemblerTest, DisassembleTest) {
  TestDisassembler d;
  CHECK(d.Init(FLAGS_test_srcdir +
               "/testdata/test.binary"));
  EXPECT_TRUE(d.DisassembleRange(0x400b80, 0x400eac));
  EXPECT_EQ(d.call_target_map_[0x400cdd], 0x404930);
  EXPECT_EQ(d.cjmp_target_map_[0x400ccd], 0x400da0);
  EXPECT_EQ(d.djmp_target_map_[0x400d0c], 0x400d2f);
  EXPECT_TRUE(d.call_target_map_.find(0x400ed0) == d.call_target_map_.end());
  EXPECT_EQ(d.terminators_[0], 0x400d81);
  std::set<uint64> addr = d.GetAddrs();
  EXPECT_EQ(addr.size(), 187);
  EXPECT_TRUE(addr.find(0x400ccd) != addr.end());
}

TEST_F(DisassemblerTest, JumpTableTest) {
  TestDisassembler d;
  CHECK(d.Init(FLAGS_test_srcdir +
               "/testdata/" +
               "jump_table_test.binary"));
  EXPECT_TRUE(d.DisassembleRange(0x40055d, 0x400628));
  EXPECT_EQ(d.idjmp_targets_map_[0x40057d].size(), 19);
  EXPECT_EQ(d.idjmp_targets_map_[0x4005db].size(), 19);
  EXPECT_EQ(d.terminators_[0], 0x400627);
  std::set<uint64> addr = d.GetAddrs();
  EXPECT_EQ(addr.size(), 56);
  EXPECT_TRUE(addr.find(0x400598) == addr.end());
}

}  // namespace
