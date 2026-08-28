#include "addr2line.h"

#include <cstdint>
#include <utility>
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
  AddressHandler() { set_addr_base(0); }

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
      low_pcs.push_back(value);
    } else if (attr == autofdo::DW_AT_high_pc) {
      high_pcs.push_back(value);
    }
  }

  std::vector<uint64> low_pcs;
  std::vector<uint64> high_pcs;
};

void AppendUint16(std::vector<unsigned char>* bytes, uint16_t value) {
  bytes->push_back(value & 0xff);
  bytes->push_back((value >> 8) & 0xff);
}

void AppendUint32(std::vector<unsigned char>* bytes, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    bytes->push_back((value >> shift) & 0xff);
  }
}

void AppendUint64(std::vector<unsigned char>* bytes, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    bytes->push_back((value >> shift) & 0xff);
  }
}

std::vector<unsigned char> MakeAddressTable(bool dwarf5) {
  constexpr uint32_t kAddressCount = 129;
  std::vector<unsigned char> bytes;
  if (dwarf5) {
    AppendUint32(&bytes, 4 + kAddressCount * sizeof(uint64_t));
    AppendUint16(&bytes, 5);
    bytes.push_back(sizeof(uint64_t));
    bytes.push_back(0);
  }

  for (uint32_t index = 0; index < kAddressCount; ++index) {
    uint64_t address = 0x1000 + index;
    if (index < 5) address = 0x1000 * (index + 1);
    if (index == 128) address = 0x9000;
    AppendUint64(&bytes, address);
  }
  return bytes;
}

struct ReaderResult {
  bool malformed;
  std::vector<uint64> low_pcs;
  std::vector<uint64> high_pcs;
};

ReaderResult ReadAddresses(const std::vector<unsigned char>& debug_info,
                           const std::vector<unsigned char>& debug_abbrev,
                           const std::vector<unsigned char>& debug_addr) {
  autofdo::SectionMap sections;
  sections[".debug_info"] = {
      reinterpret_cast<const char*>(debug_info.data()), debug_info.size()};
  sections[".debug_abbrev"] = {
      reinterpret_cast<const char*>(debug_abbrev.data()),
      debug_abbrev.size()};
  sections[".debug_addr"] = {
      reinterpret_cast<const char*>(debug_addr.data()), debug_addr.size()};

  autofdo::ByteReader reader(autofdo::ENDIANNESS_LITTLE);
  AddressHandler handler;
  autofdo::CompilationUnit unit("addrx-forms-fixture", sections, 0, &reader,
                                &handler);
  unit.Start();
  return {unit.malformed(), std::move(handler.low_pcs),
          std::move(handler.high_pcs)};
}

TEST(Addr2lineTest, Dwarf2Dwarf5Binary) {
  const std::string binary =
      absl::StrCat(::testing::SrcDir(),
                   "/testdata/"
                   "dwarf2_dwarf5.bin");

  Addr2line* addr2line = Addr2line::Create(binary);
  EXPECT_TRUE(addr2line != NULL);
}

TEST(Addr2lineTest, Dwarf5AddrxFormsConsumeEntireOperands) {
  const std::vector<unsigned char> debug_info = {
      0x23, 0x00, 0x00, 0x00,  // unit_length
      0x05, 0x00,              // version
      0x01,                    // DW_UT_compile
      0x08,                    // address_size
      0x00, 0x00, 0x00, 0x00,  // abbrev_offset
      0x01,                    // root DIE
      0x02, 0x00, 0xa1,              // addrx1 index 0 / marker
      0x03, 0x01, 0x00, 0xa2,        // addrx2 index 1 / marker
      0x04, 0x02, 0x00, 0x00, 0xa3,  // addrx3 index 2 / marker
      0x05, 0x03, 0x00, 0x00, 0x00, 0xa4,  // addrx4 index 3 / marker
      0x06, 0x04, 0xa5,              // one-byte addrx index 4 / marker
      0x07, 0x80, 0x01, 0xa6,        // two-byte addrx index 128 / marker
      0x00,                    // end root children
  };
  const std::vector<unsigned char> debug_abbrev = {
      0x01, 0x11, 0x01, 0x00, 0x00,
      0x02, 0x34, 0x00, 0x11, 0x29, 0x12, 0x0b, 0x00, 0x00,
      0x03, 0x34, 0x00, 0x11, 0x2a, 0x12, 0x0b, 0x00, 0x00,
      0x04, 0x34, 0x00, 0x11, 0x2b, 0x12, 0x0b, 0x00, 0x00,
      0x05, 0x34, 0x00, 0x11, 0x2c, 0x12, 0x0b, 0x00, 0x00,
      0x06, 0x34, 0x00, 0x11, 0x1b, 0x12, 0x0b, 0x00, 0x00,
      0x07, 0x34, 0x00, 0x11, 0x1b, 0x12, 0x0b, 0x00, 0x00,
      0x00,
  };

  const ReaderResult result =
      ReadAddresses(debug_info, debug_abbrev, MakeAddressTable(true));
  EXPECT_FALSE(result.malformed);
  EXPECT_EQ(result.low_pcs,
            (std::vector<uint64>{0x1000, 0x2000, 0x3000, 0x4000, 0x5000,
                                 0x9000}));
  EXPECT_EQ(result.high_pcs,
            (std::vector<uint64>{0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6}));
}

TEST(Addr2lineTest, Dwarf4GnuAddrIndexConsumesEntireOperand) {
  const std::vector<unsigned char> debug_info = {
      0x0d, 0x00, 0x00, 0x00,  // unit_length
      0x04, 0x00,              // version
      0x00, 0x00, 0x00, 0x00,  // abbrev_offset
      0x08,                    // address_size
      0x01,                    // root DIE
      0x02, 0x80, 0x01,        // two-byte GNU_addr_index 128
      0x7f,                    // DW_AT_high_pc
      0x00,                    // end root children
  };
  const std::vector<unsigned char> debug_abbrev = {
      0x01, 0x11, 0x01, 0x00, 0x00,
      0x02, 0x34, 0x00,
      0x11, 0x81, 0x3e,  // DW_AT_low_pc / DW_FORM_GNU_addr_index
      0x12, 0x0b,        // DW_AT_high_pc / DW_FORM_data1
      0x00, 0x00,
      0x00,
  };

  const ReaderResult result =
      ReadAddresses(debug_info, debug_abbrev, MakeAddressTable(false));
  EXPECT_FALSE(result.malformed);
  EXPECT_EQ(result.low_pcs, (std::vector<uint64>{0x9000}));
  EXPECT_EQ(result.high_pcs, (std::vector<uint64>{0x7f}));
}
}  // namespace
