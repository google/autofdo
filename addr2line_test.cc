#include "addr2line.h"

#include <array>
#include <vector>

#include "gtest/gtest.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/symbolize/addr2line_inlinestack.h"
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

TEST(Addr2lineTest, Dwarf5ImplicitConstDiscriminator) {
  // Three sibling inline instances encode discriminator 7 as implicit_const,
  // data1 and sdata. The last form remains ignored, leaving discriminator 0.
  // Their parent covers [0x1000, 0x1030).
  const unsigned char debug_info[] = {
      0x25, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00, 0x01, 0x04,  // version, unit_type, address_size
      0x00, 0x00, 0x00, 0x00,  // abbrev_offset
      0x01,                                      // compilation unit
      0x02, 0x00, 0x10, 0x00, 0x00, 0x30,       // parent
      0x03, 0x10, 0x10, 0x00, 0x00, 0x08,       // implicit_const
      0x04, 0x18, 0x10, 0x00, 0x00, 0x08, 0x07, // data1
      0x05, 0x20, 0x10, 0x00, 0x00, 0x08, 0x07, // sdata
      0x00, 0x00,                               // end children
  };
  const unsigned char debug_abbrev[] = {
      0x01, 0x11, 0x01, 0x00, 0x00,  // compilation unit with children
      0x02, 0x2e, 0x01,              // subprogram with children
      0x11, 0x01, 0x12, 0x0b,        // low_pc / addr, high_pc / data1
      0x00, 0x00,
      0x03, 0x1d, 0x00,              // inlined_subroutine without children
      0x11, 0x01, 0x12, 0x0b,
      0xb6, 0x42, 0x21, 0x07,        // GNU_discriminator / implicit_const 7
      0x00, 0x00,
      0x04, 0x1d, 0x00,              // inlined_subroutine without children
      0x11, 0x01, 0x12, 0x0b,
      0xb6, 0x42, 0x0b,              // GNU_discriminator / data1
      0x00, 0x00,
      0x05, 0x1d, 0x00,              // inlined_subroutine without children
      0x11, 0x01, 0x12, 0x0b,
      0xb6, 0x42, 0x0d,              // GNU_discriminator / sdata
      0x00, 0x00, 0x00,
  };
  autofdo::SectionMap sections;
  sections[".debug_info"] = {
      reinterpret_cast<const char*>(debug_info), sizeof(debug_info)};
  sections[".debug_abbrev"] = {
      reinterpret_cast<const char*>(debug_abbrev), sizeof(debug_abbrev)};
  autofdo::ByteReader reader(autofdo::ENDIANNESS_LITTLE);
  autofdo::InlineStackHandler handler(nullptr, sections, &reader, 0);
  autofdo::CompilationUnit unit("discriminator-fixture", sections, 0,
                                &reader, &handler);
  unit.Start();
  ASSERT_FALSE(unit.malformed());
  handler.PopulateSubprogramsByAddress();
  for (uint64 pc : {0x1010, 0x1018}) {
    SCOPED_TRACE(pc);
    const auto* info = handler.GetSubprogramForAddress(pc);
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->inlined());
    EXPECT_EQ(info->callsite_discr(), 7);
  }
  const auto* info = handler.GetSubprogramForAddress(0x1020);
  ASSERT_NE(info, nullptr);
  EXPECT_TRUE(info->inlined());
  EXPECT_EQ(info->callsite_discr(), 0);
}
}  // namespace
