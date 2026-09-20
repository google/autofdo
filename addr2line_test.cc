#include "addr2line.h"

#include <array>
#include <vector>

#include "gtest/gtest.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/symbolize/addr2line_inlinestack.h"
#include "util/symbolize/bytereader.h"
#include "util/symbolize/dwarf2reader.h"
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
}  // namespace
