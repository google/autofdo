#include "addr2line.h"

#include <array>
#include <vector>

#include "gtest/gtest.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/symbolize/addr2line_inlinestack.h"
#include "util/symbolize/bytereader.h"
#include "util/symbolize/dwarf2reader.h"
#include "util/symbolize/dwarf3ranges.h"
#include "util/symbolize/functioninfo.h"

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

TEST(Addr2lineTest, Dwarf5LineTableStringSections) {
  const unsigned char debug_line[] = {
      0x1e, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00, 0x04, 0x00,  // version, address_size, segment_selector_size
      0x13, 0x00, 0x00, 0x00,  // header_length
      0x01, 0x01, 0x01, 0x00, 0x01, 0x01,  // line program parameters
      0x01, 0x01, 0x0e, 0x01,  // one directory: path / strp
      0x00, 0x00, 0x00, 0x00,  // "dir" in .debug_str
      0x01, 0x01, 0x25, 0x01,  // one file: path / strx1
      0x00,                    // index 0 in .debug_str_offsets
      0x00, 0x01, 0x01,        // DW_LNE_end_sequence
  };
  // Distinct contents catch using one section's buffer in place of another.
  // The zero padding also makes the old, incorrect lookup safe.
  const char line_str[16] = {};
  const char str[] = "dir\0file.cc";
  const unsigned char str_offsets[] = {
      0x08, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00, 0x00, 0x00,  // version, padding
      0x04, 0x00, 0x00, 0x00,  // index 0 -> "file.cc" in .debug_str
  };
  autofdo::SectionMap sections;
  sections[".debug_line"] = {
      reinterpret_cast<const char*>(debug_line), sizeof(debug_line)};
  sections[".debug_line_str"] = {line_str, sizeof(line_str)};
  sections[".debug_str"] = {str, sizeof(str)};
  sections[".debug_str_offsets"] = {
      reinterpret_cast<const char*>(str_offsets), sizeof(str_offsets)};

  for (bool use_inline_stack : {false, true}) {
    SCOPED_TRACE(use_inline_stack);
    autofdo::ByteReader reader(autofdo::ENDIANNESS_LITTLE);
    reader.SetAddressSize(4);
    autofdo::FileVector files;
    autofdo::DirectoryVector dirs;
    autofdo::AddressToLineMap lines;
    autofdo::FunctionMap functions_by_offset, functions_by_address;
    autofdo::CULineInfoHandler line_handler(&files, &dirs, &lines);
    autofdo::CUFunctionInfoHandler function_handler(
        &files, &dirs, &lines, &functions_by_offset, &functions_by_address,
        &line_handler, sections, &reader);
    autofdo::InlineStackHandler inline_handler(nullptr, sections, &reader, 0);
    inline_handler.set_line_handler(&line_handler);
    autofdo::Dwarf2Handler* handler = &function_handler;
    if (use_inline_stack) {
      handler = &inline_handler;
    }
    handler->StartCompilationUnit(0, 4, 4, 0, 5);
    handler->StartDIE(0, autofdo::DW_TAG_compile_unit, {});
    handler->set_str_offset_base(8);
    handler->ProcessAttributeUnsigned(
        0, autofdo::DW_AT_stmt_list, autofdo::DW_FORM_sec_offset, 0);
    handler->EndDIE(0);
    ASSERT_EQ(dirs.size(), 2);
    EXPECT_STREQ(dirs[1], "dir");
    ASSERT_EQ(files.size(), 2);
    EXPECT_STREQ(files[1].second, "file.cc");
  }
}

TEST(Addr2lineTest, Dwarf5LineTableFileMetadata) {
  class FileHandler : public autofdo::LineInfoHandler {
   public:
    void DefineFile(const char* name, int32, uint32 dir, uint64 time,
                    uint64 size) override {
      ++files;
      EXPECT_STREQ(name, "file.cc");
      EXPECT_EQ(dir, 0);
      EXPECT_EQ(time, 0x12345678);
      EXPECT_EQ(size, 0x0123456789abcdefULL);
    }
    int files = 0;
  } handler;
  const unsigned char debug_line[] = {
      0x4a, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00, 0x04, 0x00,  // version, address_size, segment_selector_size
      0x3f, 0x00, 0x00, 0x00,  // header_length
      0x01, 0x01, 0x01, 0x00, 0x01, 0x01,  // line program parameters
      0x01, 0x01, 0x08, 0x01, 'd', 'i', 'r', 0,  // directory / string
      0x05,                    // five file entry fields
      0x01, 0x08,              // path / string
      0x05, 0x1e,              // MD5 / data16
      0x03, 0x06,              // timestamp / data4
      0x04, 0x07,              // size / data8
      0x02, 0x0b,              // directory_index / data1
      0x01, 'f', 'i', 'l', 'e', '.', 'c', 'c', 0,  // one file
      // MD5 precedes the integers so their values also check cursor alignment.
      0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
      0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
      0x78, 0x56, 0x34, 0x12,
      0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
      0x00,                    // directory index
      0x00, 0x01, 0x01,        // DW_LNE_end_sequence
  };
  autofdo::ByteReader reader(autofdo::ENDIANNESS_LITTLE);
  reader.SetAddressSize(4);
  autofdo::LineInfo lines(reinterpret_cast<const char*>(debug_line),
                          sizeof(debug_line), &reader, &handler);
  EXPECT_EQ(lines.Start(), sizeof(debug_line));
  EXPECT_FALSE(lines.malformed());
  EXPECT_EQ(handler.files, 1);

  // Cut off MD5, timestamp and size one byte early, respectively.
  for (size_t length : {61, 65, 73}) {
    SCOPED_TRACE(length);
    std::vector<unsigned char> data(debug_line, debug_line + length);
    data[0] = length - 4;
    autofdo::LineInfo truncated(reinterpret_cast<const char*>(data.data()),
                               data.size(), &reader, &handler);
    truncated.Start();
    EXPECT_TRUE(truncated.malformed());
  }
}
}  // namespace
