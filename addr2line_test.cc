#include "addr2line.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/symbolize/addr2line_inlinestack.h"
#include "util/symbolize/bytereader.h"
#include "util/symbolize/dwarf2reader.h"
#include "util/symbolize/dwarf3ranges.h"

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

TEST(Addr2lineTest, Dwarf5RnglistsAbsoluteEntriesIgnoreBase) {
  const unsigned char debug_rnglists[] = {
      0x1c, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00, 0x08, 0x00,  // version, address_size, segment_selector_size
      0x00, 0x00, 0x00, 0x00,  // offset_entry_count
      autofdo::DW_RLE_startx_endx, 0x00, 0x01,     // [0x2000, 0x2020)
      autofdo::DW_RLE_startx_length, 0x01, 0x20,   // [0x2020, 0x2040)
      autofdo::DW_RLE_start_length,               // [0x2040, 0x2060)
      0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
      autofdo::DW_RLE_offset_pair, 0x40, 0x60,    // [base + 0x40, base + 0x60)
      autofdo::DW_RLE_end_of_list,
  };
  const unsigned char debug_addr[] = {
      0x14, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00, 0x08, 0x00,  // version, address_size, segment_selector_size
      0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x2000
      0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x2020
  };
  autofdo::ByteReader reader(autofdo::ENDIANNESS_LITTLE);
  reader.SetAddressSize(8);
  reader.SetOffsetSize(4);
  autofdo::AddressRangeList ranges(
      nullptr, 0, reinterpret_cast<const char*>(debug_rnglists),
      sizeof(debug_rnglists), &reader, reinterpret_cast<const char*>(debug_addr),
      sizeof(debug_addr));

  // A zero CU base masks the bug. Absolute entries must also work with a
  // nonzero base, while the offset-pair control must continue to use it.
  for (uint64 base : {0, 0x1000}) {
    SCOPED_TRACE(base);
    autofdo::AddressRangeList::RangeList actual;
    ranges.ReadDwarfRngListsDirectly(12, base, &actual, 8);
    const autofdo::AddressRangeList::RangeList expected = {
        {0x2000, 0x2020}, {0x2020, 0x2040}, {0x2040, 0x2060},
        {base + 0x40, base + 0x60}};
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(actual[i], expected[i]) << "range " << i;
    }
  }
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

class StringHandler : public autofdo::Dwarf2Handler {
 public:
  bool StartCompilationUnit(uint64, uint8, uint8, uint64, uint8) override {
    return true;
  }

  bool StartDIE(uint64, autofdo::DwarfTag,
                const autofdo::AttributeList&) override {
    return true;
  }

  void ProcessAttributeString(uint64, autofdo::DwarfAttribute attr,
                              autofdo::DwarfForm, const char* data) override {
    if (attr == autofdo::DW_AT_name) name = data ? data : "<null>";
  }

  void ProcessAttributeUnsigned(uint64, autofdo::DwarfAttribute attr,
                                autofdo::DwarfForm, uint64 value) override {
    if (attr == autofdo::DW_AT_high_pc) high_pc = value;
  }

  std::string name;
  uint64 high_pc = 0;
};

// A DWARF 5 unit DIE whose DW_AT_name (DW_FORM_strx1, index |str_index|)
// precedes its DW_AT_str_offsets_base, as clang emits it. The base selects
// the second of two .debug_str_offsets contributions. DW_AT_high_pc is a
// marker that shows the strx1 operand was consumed.
void ParseStrxUnit(bool with_str_offsets, unsigned char str_index,
                   StringHandler* handler) {
  const unsigned char debug_info[] = {
      0x0f, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00,              // version
      0x01,                    // DW_UT_compile
      0x08,                    // address_size
      0x00, 0x00, 0x00, 0x00,  // abbrev_offset
      0x01,                    // compilation unit
      str_index,               // DW_AT_name / strx1
      0x14, 0x00, 0x00, 0x00,  // DW_AT_str_offsets_base: second contribution
      0x7f,                    // DW_AT_high_pc marker
  };
  const unsigned char debug_abbrev[] = {
      0x01, 0x11, 0x00,  // compilation unit without children
      0x03, 0x25,        // DW_AT_name / DW_FORM_strx1
      0x72, 0x17,        // DW_AT_str_offsets_base / DW_FORM_sec_offset
      0x12, 0x0b,        // DW_AT_high_pc / DW_FORM_data1
      0x00, 0x00, 0x00,
  };
  const unsigned char debug_str_offsets[] = {
      0x08, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,  // first unit header
      0x00, 0x00, 0x00, 0x00,                          // "wrong"
      0x08, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,  // second unit header
      0x06, 0x00, 0x00, 0x00,                          // "right"
  };
  const char debug_str[] = "wrong\0right";
  autofdo::SectionMap sections;
  sections[".debug_info"] = {
      reinterpret_cast<const char*>(debug_info), sizeof(debug_info)};
  sections[".debug_abbrev"] = {
      reinterpret_cast<const char*>(debug_abbrev), sizeof(debug_abbrev)};
  sections[".debug_str"] = {debug_str, sizeof(debug_str)};
  if (with_str_offsets) {
    sections[".debug_str_offsets"] = {
        reinterpret_cast<const char*>(debug_str_offsets),
        sizeof(debug_str_offsets)};
  }
  autofdo::ByteReader reader(autofdo::ENDIANNESS_LITTLE);
  autofdo::CompilationUnit unit("strx-fixture", sections, 0, &reader, handler);
  unit.Start();
  EXPECT_FALSE(unit.malformed());
  EXPECT_EQ(handler->high_pc, 0x7f);
}

TEST(Addr2lineTest, Dwarf5StrxUsesLaterStrOffsetsBase) {
  StringHandler handler;
  ParseStrxUnit(/*with_str_offsets=*/true, /*str_index=*/0, &handler);
  EXPECT_EQ(handler.name, "right");
}

TEST(Addr2lineTest, Dwarf5StrxWithoutStrOffsetsSectionIsSkipped) {
  StringHandler handler;
  ParseStrxUnit(/*with_str_offsets=*/false, /*str_index=*/0, &handler);
  EXPECT_EQ(handler.name, "");
}

TEST(Addr2lineTest, Dwarf5StrxOutOfRangeIndexIsSkipped) {
  StringHandler handler;
  ParseStrxUnit(/*with_str_offsets=*/true, /*str_index=*/1, &handler);
  EXPECT_EQ(handler.name, "");
}

// testdata/dwarf5_line_tables_only.bin is built by clang 19 from
// testdata/dwarf5_line_tables_only_{main,work,split}.c with
//   clang -O2 -gdwarf-5 -gline-tables-only -fdebug-info-for-profiling
//     -fdebug-prefix-map=$PWD=/autofdo/testdata
// It uses .debug_str_offsets, .debug_addr, .debug_rnglists, .debug_line_str
// and DW_LNCT_MD5 file entries with 0-based file indices.
TEST(Addr2lineTest, Dwarf5ClangLineTablesOnlyBinary) {
  const std::string binary = absl::StrCat(
      ::testing::SrcDir(), "/testdata/dwarf5_line_tables_only.bin");
  std::unique_ptr<Addr2line> addr2line(Addr2line::Create(binary));
  ASSERT_NE(addr2line, nullptr);

  struct Frame {
    std::string func_name;
    std::string file_name;
    uint32_t line;
    uint32_t start_line;
    uint32_t discriminator;
  };
  const struct {
    uint64_t address;
    std::vector<Frame> frames;
  } kCases[] = {
      // square_add inlined into main (first unit).
      {0x118a,
       {{"square_add", "dwarf5_line_tables_only_main.c", 9, 8, 0},
        {"main", "dwarf5_line_tables_only_main.c", 16, 12, 2}}},
      // mix inlined into work (second unit). The inline ranges are offsets
      // from this unit's DW_AT_low_pc, a DW_FORM_addrx emitted before
      // DW_AT_addr_base.
      {0x1221,
       {{"mix", "dwarf5_line_tables_only_work.c", 3, 2, 97},
        {"work", "dwarf5_line_tables_only_work.c", 9, 6, 0}}},
      {0x11c0, {{"work", "dwarf5_line_tables_only_work.c", 6, 6, 0}}},
      // Third unit, described with DW_AT_ranges / DW_FORM_rnglistx.
      {0x1320, {{"split", "dwarf5_line_tables_only_split.c", 9, 8, 0}}},
      {0x1330,
       {{"split_cold", "dwarf5_line_tables_only_split.c", 5, 4, 0}}},
  };
  for (const auto& test_case : kCases) {
    SCOPED_TRACE(absl::StrCat("address 0x", absl::Hex(test_case.address)));
    devtools_crosstool_autofdo::SourceStack stack;
    addr2line->GetInlineStack(test_case.address, &stack);
    ASSERT_EQ(stack.size(), test_case.frames.size());
    for (size_t i = 0; i < stack.size(); ++i) {
      SCOPED_TRACE(i);
      const Frame& expected = test_case.frames[i];
      ASSERT_NE(stack[i].func_name, nullptr);
      EXPECT_EQ(stack[i].func_name, expected.func_name);
      EXPECT_EQ(stack[i].dir_name, "/autofdo/testdata");
      EXPECT_EQ(stack[i].file_name, expected.file_name);
      EXPECT_EQ(stack[i].line, expected.line);
      EXPECT_EQ(stack[i].start_line, expected.start_line);
      EXPECT_EQ(stack[i].discriminator, expected.discriminator);
    }
  }
}
}  // namespace
