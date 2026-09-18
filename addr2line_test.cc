#include "addr2line.h"

#include <array>
#include <vector>

#include "gtest/gtest.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/symbolize/bytereader.h"
#include "util/symbolize/dwarf2reader.h"

namespace {

using ::devtools_crosstool_autofdo::Addr2line;

namespace autofdo = devtools_crosstool_autofdo;

class AddressHandler : public autofdo::Dwarf2Handler {
 public:
  bool StartCompilationUnit(uint64, uint8, uint8, uint64, uint8) override {
    return true;
  }

  bool StartDIE(uint64, autofdo::DwarfTag,
                const autofdo::AttributeList&) override {
    return true;
  }

  void ProcessAttributeUnsigned(uint64, autofdo::DwarfAttribute attr,
                                autofdo::DwarfForm, uint64 value) override {
    if (attr == autofdo::DW_AT_low_pc) {
      low_pc = value;
    } else if (attr == autofdo::DW_AT_high_pc) {
      high_pc = value;
    }
  }

  uint64 low_pc = 0;
  uint64 high_pc = 0;
};

// Both encodings select address-table entry 128. Checking low_pc catches an
// incorrectly decoded index; high_pc checks that both operand bytes were read.
// The marker's trailing zero keeps a short read from looking like another DIE.
void ExpectIndexedAddress(autofdo::DwarfForm form,
                          std::array<unsigned char, 2> operand) {
  const std::vector<unsigned char> debug_info = {
      0x0f, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00,              // version
      0x01,                    // DW_UT_compile
      0x08,                    // address_size
      0x00, 0x00, 0x00, 0x00,  // abbrev_offset
      0x01,                    // root DIE
      0x02, operand[0], operand[1], 0x7f, 0x00,  // index 128 / high_pc marker
      0x00,                    // end root children
  };
  const std::vector<unsigned char> debug_abbrev = {
      0x01, 0x11, 0x01, 0x00, 0x00,  // compilation unit with children
      0x02, 0x2e, 0x00,              // subprogram without children
      0x11, static_cast<unsigned char>(form),  // DW_AT_low_pc
      0x12, 0x05,                    // DW_AT_high_pc / DW_FORM_data2
      0x00, 0x00, 0x00,
  };
  std::vector<unsigned char> debug_addr = {
      0x0c, 0x04, 0x00, 0x00,  // unit_length: 4 + 129 * 8
      0x05, 0x00, 0x08, 0x00,  // version, address_size, segment_selector_size
  };
  for (uint64 address = 0x1000; address <= 0x1080; ++address) {
    for (int shift = 0; shift < 64; shift += 8) {
      debug_addr.push_back((address >> shift) & 0xff);
    }
  }

  autofdo::SectionMap sections;
  sections[".debug_info"] = {
      reinterpret_cast<const char*>(debug_info.data()), debug_info.size()};
  sections[".debug_abbrev"] = {
      reinterpret_cast<const char*>(debug_abbrev.data()), debug_abbrev.size()};
  sections[".debug_addr"] = {
      reinterpret_cast<const char*>(debug_addr.data()), debug_addr.size()};

  autofdo::ByteReader reader(autofdo::ENDIANNESS_LITTLE);
  AddressHandler handler;
  autofdo::CompilationUnit unit("addrx-fixture", sections, 0, &reader, &handler);
  unit.Start();
  EXPECT_FALSE(unit.malformed());
  EXPECT_EQ(handler.low_pc, 0x1080);
  EXPECT_EQ(handler.high_pc, 0x7f);
}

TEST(Addr2lineTest, Dwarf2Dwarf5Binary) {
  const std::string binary =
      absl::StrCat(::testing::SrcDir(),
                   "/testdata/"
                   "dwarf2_dwarf5.bin");

  Addr2line* addr2line = Addr2line::Create(binary);
  EXPECT_TRUE(addr2line != NULL);
}

TEST(Addr2lineTest, Dwarf5Addrx2ReadsIndexAndConsumesOperand) {
  ExpectIndexedAddress(autofdo::DW_FORM_addrx2, {0x80, 0x00});
}

TEST(Addr2lineTest, Dwarf5AddrxConsumesMultibyteOperand) {
  ExpectIndexedAddress(autofdo::DW_FORM_addrx, {0x80, 0x01});
}
}  // namespace
