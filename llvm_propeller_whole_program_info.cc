#include "llvm_propeller_whole_program_info.h"

#include <fcntl.h>  // for "O_RDONLY"

#include <algorithm>
#include <cstdint>
#include <future>  // NOLINT(build/c++11)
#include <numeric>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <tuple>
#include <type_traits>
#include <utility>

#include "llvm_propeller_formatting.h"
#include "llvm_propeller_options.pb.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"

namespace devtools_crosstool_autofdo {

namespace {
using ::llvm::Expected;
using ::llvm::Optional;
using ::llvm::StringRef;
using ::llvm::object::ObjectFile;

// Section .llvm_bb_addr_map consists of many AddrMapEntry.
struct AddrMapEntry {
  struct BbInfo {
    uint64_t offset;
    uint64_t size;
    uint64_t meta;
  };

  uint64_t func_address;
  std::vector<BbInfo> bb_info;
};

// Read a uleb field value and advance *p.
template <class ValueType>
bool ReadUlebField(absl::string_view field_name, const uint8_t **p,
                   const unsigned char *end_mark, ValueType *v) {
  const char *err = nullptr;
  unsigned n = 0;
  *v = llvm::decodeULEB128(*p, &n, end_mark, &err);
  if (err != nullptr) {
    LOG(ERROR) << absl::StreamFormat("Read .bb_info %s uleb128 error: %s",
                                     field_name, err);
    return false;
  }
  *p += n;
  return true;
}

// Read one bbaddrmap entry from *p, make sure never move *p beyond end.
absl::optional<AddrMapEntry> ReadOneEntry(const uint8_t **p,
                                          const uint8_t * const end) {
  const uint64_t address = *(reinterpret_cast<const uint64_t *>(*p));
  *p += sizeof(uint64_t);
  uint64_t num_blocks = 0, meta = 0, size = 0, offset = 0, previous_offset = 0;
  if (!ReadUlebField("bb count", p, end, &num_blocks)) return {};
  std::vector<AddrMapEntry::BbInfo> blocks;
  for (int ib = 0; ib < num_blocks; ++ib) {
    previous_offset = offset;
    if (ReadUlebField("bb offset", p, end, &offset) &&
        ReadUlebField("bb size", p, end, &size) &&
        ReadUlebField("bb metadata", p, end, &meta)) {
      // Check assumption here: bb records appear in the order of "symbol
      // offset".
      CHECK(previous_offset <= offset);
      blocks.push_back({offset, size, meta});
    }
  }
  return AddrMapEntry{address, blocks};
}

llvm::Optional<llvm::object::SectionRef> FindBbAddrMapSection(
    const llvm::object::ObjectFile &obj) {
  for (auto sec : obj.sections()) {
    Expected<llvm::StringRef> sn = sec.getName();
    llvm::object::ELFSectionRef esec(sec);
#if LLVM_VERSION_MAJOR >= 12
    if (sn && esec.getType() == llvm::ELF::SHT_LLVM_BB_ADDR_MAP &&
        (*sn) == PropellerWholeProgramInfo::kBbAddrMapSectionName)
#else
    if (sn && (*sn) == PropellerWholeProgramInfo::kBbAddrMapSectionName)
#endif
      return sec;
  }
  return llvm::None;
}
}  // namespace

bool ReadTextSectionAddresses(ObjectFile &obj);

std::unique_ptr<PropellerWholeProgramInfo> PropellerWholeProgramInfo::Create(
    const PropellerOptions &options) {
  BinaryPerfInfo bpi;
  if (!PerfDataReader().SelectBinaryInfo(options.binary_name(),
                                         &bpi.binary_info))
    return nullptr;

  Optional<llvm::object::SectionRef> opt_bb_addr_map_section =
      FindBbAddrMapSection(*(bpi.binary_info.object_file));

  ReadTextSectionAddresses(*(bpi.binary_info.object_file));

  if (!opt_bb_addr_map_section) {
    LOG(ERROR) << "'" << options.binary_name() << "' does not have '"
              << kBbAddrMapSectionName << "' section.";
    return nullptr;
  }
  // Do not use std::make_unique, ctor is not public.
  return std::unique_ptr<PropellerWholeProgramInfo>(
      new PropellerWholeProgramInfo(options, std::move(bpi),
                                    *opt_bb_addr_map_section));
}

SymbolEntry *PropellerWholeProgramInfo::CreateFuncSymbolEntry(
    uint64_t ordinal, StringRef func_name, SymbolEntry::AliasesTy aliases,
    int func_bb_num, uint64_t address, uint64_t size) {
  if (bb_addr_map_.find(func_name) != bb_addr_map_.end()) {
    LOG(ERROR) << "Found duplicate function symbol '" << func_name.str() << "'@"
               << AddressFormatter(address)
               << ", drop the symbol and all its bb symbols.";
    ++stats_.duplicate_symbols;
    return nullptr;
  }
  // Pre-allocate "func_bb_num" nullptrs in bb_addr_map_[func_name].
  bb_addr_map_.emplace(std::piecewise_construct,
                       std::forward_as_tuple(func_name),
                       std::forward_as_tuple(func_bb_num, nullptr));
  // Note, funcsym is not put into bb_addr_map_. It's accessible via
  // bb_addr_map_[func_name].front()->func_ptr.
  // Put into address_map_.
  address_map_[address].emplace_back(std::make_unique<SymbolEntry>(
      ordinal, func_name, aliases, address, size, nullptr, 0));
  ++stats_.syms_created;
  return address_map_[address].back().get();
}

SymbolEntry *PropellerWholeProgramInfo::CreateBbSymbolEntry(
    uint64_t ordinal, SymbolEntry *parent_func, int bb_index, uint64_t address,
    uint64_t size, uint32_t metadata) {
  CHECK(parent_func);
  // BB symbol. We put it into: bb_addr_map_[parent_func->name][bbid].
  BbAddrMapTy::iterator iter = bb_addr_map_.find(parent_func->name);
  if (iter == bb_addr_map_.end()) {
    LOG(ERROR) << "BB symbol '" << parent_func->name.str() << "." << bb_index
               << "'@" << AddressFormatter(address)
               << " appears before its function record, drop the symbol.";
    return nullptr;
  }
  // Note - bbsym does not have a field for bbindex, because the name field in
  // SymbolEntry is StringRef and in bbinfo workflow, the bbsym's name field is
  // always "". But bbsym's id number can be calculated from: (bbsym->ordinal -
  // bbsym->func_ptr->ordinal - 1).
  auto bb_symbol =
      std::make_unique<SymbolEntry>(ordinal, "", SymbolEntry::AliasesTy(),
                                    address, size, parent_func, metadata);
  CHECK(bb_index < iter->second.size());
  iter->second[bb_index] = bb_symbol.get();
  // Put into address_map_.
  address_map_[address].emplace_back(std::move(bb_symbol));
  ++stats_.syms_created;
  return address_map_[address].back().get();
}

void PropellerWholeProgramInfo::ReadSymbolTable() {
  for (llvm::object::SymbolRef sr :
       binary_perf_info_.binary_info.object_file->symbols()) {
    llvm::object::ELFSymbolRef symbol(sr);
    uint8_t stt = symbol.getELFType();
    if (stt != llvm::ELF::STT_FUNC) continue;
    Expected<uint64_t> address = sr.getAddress();
    if (!address || !*address) continue;
    Expected<StringRef> func_name = symbol.getName();
    if (!func_name) continue;
    const uint64_t func_size = symbol.getSize();
    if (func_size == 0) continue;

    auto &addr_sym_list = symtab_[*address];
    // Check whether there are already symbols on the same address, if so make
    // sure they have the same size and thus they can be aliased.
    bool check_size_ok = true;
    for (auto &sym_ref : addr_sym_list) {
      uint64_t sym_size = llvm::object::ELFSymbolRef(sym_ref).getSize();
      if (func_size != sym_size) {
        LOG(WARNING) << "Multiple function symbols on the same address with "
                        "different size: "
                     << AddressFormatter(*address) << ": '" << func_name->str()
                     << "(" << func_size << ")' and '"
                     << llvm::cantFail(sym_ref.getName()).str() << "("
                     << sym_size << ")', the former will be dropped.";
        check_size_ok = false;
        break;
      }
    }
    if (check_size_ok) addr_sym_list.push_back(sr);
  }
}

std::list<llvm::object::ELFSectionRef> text_sections;

bool ReadTextSectionAddresses(ObjectFile &obj) {
  for (auto sec : obj.sections()) {
    llvm::object::ELFSectionRef esec(sec);
    if (esec.isText())
      text_sections.push_back(esec);
  }
  return true;
}

typedef uint64_t (*target_address_func_ty)(uint64_t pc, const unsigned char *rel);

uint64_t target_address_no_addr(uint64_t pc, const unsigned char *rel) {
  return 0;
}

uint64_t target_address_func_rel8(uint64_t pc, const unsigned char *rel) {
  unsigned char offset = (unsigned char)(rel[0]);
  if ((offset & 0x80) == 0) {
    return pc + offset;
  }
  offset = (~offset) + 1;
  return pc - offset;
}

uint64_t target_address_func_rel16(uint64_t pc, const unsigned char *rel) {
  unsigned short offset = (unsigned short)(rel[0] | (rel[1] << 8));
  if ((offset & 0x8000) == 0)
    return pc + offset;
  offset = (~offset) + 1;
  return pc - offset;
}

uint64_t target_address_func_rel32(uint64_t pc, const unsigned char *rel) {
  unsigned int offset = (unsigned int)(rel[0] | (rel[1] << 8) |
                                         (rel[2] << 16) | (rel[3] << 24));
  if ((offset & 0x80000000) == 0)
    return pc + offset;
  offset = (~offset) + 1;
  return pc - offset;
}

constexpr struct {
  unsigned pos;
  unsigned size;
  unsigned char p[6];
  char insn[64];
  target_address_func_ty target_func;
  unsigned max_pass;
} bb_ending_patterns[] = {
  { 6, 2, {0x0f, 0x8f}, "jg rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x8e}, "jle rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x8d}, "jge rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x8c}, "jnge rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x8b}, "jpo rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x8a}, "jp rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x89}, "jns rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x88}, "js rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x87}, "ja rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x86}, "jbe rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x85}, "jne rel32", target_address_func_rel32, 2 },
  { 6, 2, {0x0f, 0x84}, "jz rel32", target_address_func_rel32, 2},
  { 6, 2, {0x0f, 0x83}, "jae rel32", target_address_func_rel32, 2},
  { 6, 2, {0x0f, 0x82}, "jb rel32", target_address_func_rel32, 2},
  { 6, 2, {0x0f, 0x81}, "jno rel32", target_address_func_rel32, 2},
  { 5, 1, {0xe9}, "jmp rel32", target_address_func_rel32, 1 },
  { 5, 1, {0xe8}, "call rel32", target_address_no_addr, 1 },
  { 4, 2, {0x0f, 0x86}, "jbe rel16", target_address_func_rel16, 2 },
  { 4, 2, {0x0f, 0x85}, "jne rel16", target_address_func_rel16, 2 },
  { 4, 2, {0x0f, 0x84}, "jz rel16", target_address_func_rel16, 2 },
  { 4, 2, {0x0f, 0x82}, "jb rel16", target_address_func_rel16, 2 },
  { 3, 1, {0xe9}, "jmp rel16", target_address_func_rel16, 1 },
  { 2, 1, {0xeb}, "jmp rel8", target_address_func_rel8, 1 },
  { 2, 1, {0x7f}, "jg rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x7e}, "jle rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x7d}, "jge rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x7c}, "jl rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x7b}, "jnp rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x7a}, "jp rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x79}, "jns rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x78}, "js rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x77}, "ja rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x76}, "jbe rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x75}, "jnz rel8", target_address_func_rel8, 2},
  { 2, 1, {0x74}, "jz rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x73}, "jnc rel8", target_address_func_rel8, 2 },
  { 2, 1, {0x72}, "jc rel8", target_address_func_rel8, 2 },
  { 1, 1, {0xc3}, "ret", target_address_no_addr, 1 }
};
  
// cannot fix this function: static const std::string focus_func_name = "_ZN4llvm19getUnderlyingObjectEPKNS_5ValueEj";
// static const std::string focus_func_name = "_ZN4llvm5Value11setMetadataEjPNS_6MDNodeE";
// static const std::string focus_func_name = "_ZN4llvm14SpillPlacement8addLinksENS_8ArrayRefIjEE";
// static const std::string focus_func_name = "_ZN4llvm14SpillPlacement7iterateEv";
// static const std::string focus_func_name = "_ZN4llvm15LowerDbgDeclareERNS_8FunctionE";
// static const std::string focus_func_name = "_ZL15SimplifyGEPInstPN4llvm4TypeENS_8ArrayRefIPNS_5ValueEEEbRKNS_13SimplifyQueryEj.__uniq.211684107414781178935122691899976471211";

static const std::string focus_func_name = "";

bool PropellerWholeProgramInfo::CalculateStaticCfgInfo(
    const std::vector<SymbolEntry *> &basicblocks) {
  std::string func_name = basicblocks[0]->func_ptr->name.str();      
  if (focus_func_name != "" && func_name != focus_func_name)
    return false;
  auto find_containing_section =
      [](uint64_t address) -> llvm::Optional<llvm::object::ELFSectionRef> {
    for (llvm::object::ELFSectionRef &se : text_sections)
      if (se.getAddress() <= address &&
          address <= se.getAddress() + se.getSize())
        return se;
    return llvm::None;
  };
  auto opt_section = find_containing_section(basicblocks[0]->addr);
  if (!opt_section) return false;
  llvm::object::ELFSectionRef section = *opt_section;
  StringRef contents = llvm::cantFail(section.getContents());
  auto target_addr_to_bb = [&basicblocks](uint64_t addr) -> SymbolEntry * {
    for (auto *bb : basicblocks)
      if (bb->addr == addr)
        return bb;
    return nullptr;
  };
  std::map<unsigned, std::set<unsigned>> cfg;
  // fprintf(stderr, ">>> %s\n", func_name.c_str());
  for (auto *bb : basicblocks) {
    // bb index, starting from 0
    unsigned bbidx = bb->ordinal - bb->func_ptr->ordinal - 1;
    cfg[bbidx] = std::set<unsigned>();
    if (bb->CanFallThrough())
      cfg[bbidx].insert(bbidx + 1);
    bool match;
    unsigned total_matched_size = 0;
    for (unsigned pass = 1; pass <= 2; ++pass) {
      uint64_t bb_end_scan_pos = bb->addr + bb->size - total_matched_size;
      for (const auto &pat : bb_ending_patterns) {
        match = false;
        // if (bbidx == 80) {
        //   fprintf(stderr, "bb->size=%d, total_matched_size=%d, pat.size=%d\n", bb->size, total_matched_size, pat.size);
        // }
        if (bb->size - total_matched_size < pat.pos) continue;
        /*
 4a0a09a:       39 d1                   cmp    %edx,%ecx
 4a0a09c:       0f 85 3a fe ff ff       jne    4a09edc <_ZN4llvm14SpillPlacement8addLinksENS_8ArrayRefIjEE+0x1ac>
 4a0a0a2:       e9 08 fe ff ff          jmp    4a09eaf <_ZN4llvm14SpillPlacement8addLinksENS_8ArrayRefIjEE+0x17f>
        */
        if (bb->size - total_matched_size - pat.pos == 2 &&
            ((unsigned char)(contents.data()[bb->addr - section.getAddress()]) != 0xa8) &&
            ((unsigned char)(contents.data()[bb->addr - section.getAddress()]) != 0x39) &&
            ((unsigned char)(contents.data()[bb->addr - section.getAddress()]) != 0x38) &&
            ((unsigned char)(contents.data()[bb->addr - section.getAddress()]) != 0x3c) &&
            ((unsigned char)(contents.data()[bb->addr - section.getAddress()]) != 0x72) &&
            ((unsigned char)(contents.data()[bb->addr - section.getAddress()]) != 0x84) &&
            ((unsigned char)(contents.data()[bb->addr - section.getAddress()]) != 0x85))
            continue;
        // if (bb->size - total_matched_size - 3 /* -3 is heuristic, a block should have at least 3 bytes besides jmp/jcc. */ < pat.pos)
        //   continue;
        if (pat.max_pass < pass)
          continue;
        match = true;
        
        for (int i = 0; match && i < pat.size; ++i) {
          if (pat.p[i] != (unsigned char)(contents.data()[bb_end_scan_pos - pat.pos + i - section.getAddress()]))
            match = false;
        }
        if (match) {
          SymbolEntry * target_bb = nullptr;
          if (pat.target_func) {
            unsigned target_addr = pat.target_func(
                bb_end_scan_pos,
                (unsigned char *)(contents.data() + bb->addr + bb->size -
                                  total_matched_size - pat.pos + pat.size -
                                  section.getAddress()));
            if (target_addr != 0) {
              target_bb = target_addr_to_bb(target_addr);
              if (target_bb) {
                cfg[bbidx].insert(target_bb->ordinal - target_bb->func_ptr->ordinal - 1);
              } else {
                // No BB starting with "target_addr", consider this is not a match and try next.
                continue;
              }
            }
          }
          if (focus_func_name != "") {
            if (target_bb)
              fprintf(stderr, "bb.%lu: [0x%lx, 0x%lx): 0x%lx: %s (bb.%u)\n",
                      bbidx, bb->addr, bb->addr + bb->size,
                      bb->addr + bb->size - total_matched_size - pat.pos,
                      pat.insn,
                      target_bb->ordinal - target_bb->func_ptr->ordinal - 1);
            else 
              fprintf(stderr, "bb.%lu: [0x%lx, 0x%lx): 0x%lx: %s\n",
                      bbidx, bb->addr, bb->addr + bb->size,
                      bb->addr + bb->size - total_matched_size - pat.pos,
                      pat.insn);
          }
          total_matched_size += pat.pos;
          break;  // out of pattern matching
        }
      }
      if (!match) {
        if (focus_func_name != "")
          fprintf(stderr, "bb.%lu: [0x%lx, 0x%lx)\n", bbidx, bb->addr, bb->addr + bb->size);
        break;
      }
    } // end of 2 pass
    fprintf(stderr, "> %s.bb.%lu:", func_name.c_str(), bbidx);
    for (unsigned target_bbidx : cfg[bbidx])
      fprintf(stderr, " %d", target_bbidx);
    fprintf(stderr, "\n");
  }
  return true;
}

bool PropellerWholeProgramInfo::ReadBbAddrMapSection() {
  llvm::Expected<StringRef> exp_contents = bb_addr_map_section_.getContents();
  if (!exp_contents) {
    LOG(ERROR) << "Error accessing '" << kBbAddrMapSectionName
               << "' section content.";
    return false;
  }
  StringRef sec_contents = *exp_contents;
  const unsigned char * p = sec_contents.bytes_begin();
  const unsigned char * const end = sec_contents.bytes_end();

  uint64_t ordinal = 0;
  std::set<llvm::StringRef> conflicting_symbols;
  auto remove_conflicting_symbols = [this, &conflicting_symbols]() {
    uint32_t total_symbols_removed = 0;
    for (llvm::StringRef conflicting_name : conflicting_symbols) {
      BbAddrMapTy::iterator i = bb_addr_map_.find(conflicting_name);
      CHECK(i != bb_addr_map_.end());
      std::vector<SymbolEntry *> symbols_to_remove(std::move(i->second));
      bb_addr_map_.erase(i);
      // "symbols_to_remove" now contains all the BB symbols.
      CHECK(!symbols_to_remove.empty());
      // Add function symbol to "symbols_to_remove".
      symbols_to_remove.push_back(symbols_to_remove.front()->func_ptr);
      // "symbols_to_remove" contains the function symbol + all its bb symbols.
      for (SymbolEntry *symbol_to_remove : symbols_to_remove) {
        CHECK(symbol_to_remove);
        AddressMapTy::iterator addri =
            address_map_.find(symbol_to_remove->addr);
        CHECK(addri != address_map_.end());
        bool removed = false;
        llvm::SmallVector<std::unique_ptr<SymbolEntry>, 2> &addrlist =
            addri->second;
        for (auto symi = addrlist.begin(), syme = addrlist.end();
             !removed && symi != syme; ++symi) {
          if ((*symi).get() == symbol_to_remove) {
            addrlist.erase(symi);
            removed = true;
            break;
          }
        }
        CHECK(removed);
        if (addrlist.empty())
          address_map_.erase(addri);
      }
      total_symbols_removed += symbols_to_remove.size();
    }
    LOG(INFO) << "Dropped " << total_symbols_removed
              << " function and basicblock symbols because of conflicting "
                 "function name.";
  };
  auto check_conflicting_symbols =
      [this, &conflicting_symbols](
          uint64_t func_size,
          uint32_t func_num_blocks,
          llvm::SmallVector<llvm::object::SymbolRef, 2> &symbol_refs) {
        bool conflicting_symbols_found = false;
        for (llvm::object::SymbolRef sr : symbol_refs) {
          StringRef alias = llvm::cantFail(sr.getName());
          BbAddrMapTy::iterator i = bb_addr_map_.find(alias);
          if (i == bb_addr_map_.end()) continue;
          // Conflicting symbol found, check alias does not have "__uniq" part
          // and symbol_ref is not local. (bindings could be GLOBALE, LOCAL,
          // WEAK, etc)
          if (llvm::object::ELFSymbolRef(sr).getBinding() ==
              llvm::ELF::STB_GLOBAL) {
            LOG(INFO) << "Found global symbol '" << alias.str()
                      << "' conflicting with LOCAL symbol.";
          }
          conflicting_symbols_found = true;
          ++stats_.duplicate_symbols;
          if (alias.find("__uniq") != std::string::npos) {
            // duplicate uniq-named symbols found
            CHECK(!i->second.empty());
            SymbolEntry *dup_func = i->second.front()->func_ptr;
            if (dup_func->size == func_size &&
                i->second.size() == func_num_blocks) {
              // uniq-named functions found with same size and same number of
              // basicblocks, we assume they are the same and thus we keep one
              // copy of them.
              LOG(WARNING) << "duplicate uniq-named functions '" << alias.str()
                           << "' with same size and structure found.";
              continue;
            } else {
              LOG(WARNING) << "duplicate uniq-named functions '" << alias.str()
                           << "' with different size or structure found , drop "
                              "all of them.";
            }
          }
          conflicting_symbols.insert(alias);
        }
        return conflicting_symbols_found;
      };
  while (p != end) {
    // ReadOneEntry moves p ahead ...
    auto entry = ReadOneEntry(&p, end);
    // Fail to read bbinfo section in the middle is irrecoverable.
    if (!entry.has_value()) return false;

    const uint64_t func_address = entry.value().func_address;
    auto iter = symtab_.find(func_address);
    // b/169962287 under some circumstances, bb_addr_map's symbol may not have
    // associated symbols.
    if (iter == symtab_.end()) {
      LOG(WARNING) << "Invalid entry inside '" << kBbAddrMapSectionName
                   << "', at offset: " << std::showbase << std::hex
                   << p - sec_contents.bytes_begin()
                   << ". Function address listed: " << std::showbase << std::hex
                   << func_address;
      ++stats_.bbaddrmap_function_does_not_have_symtab_entry;
      continue;
    }
    SymbolEntry::AliasesTy func_aliases;
    // TODO(b/185956991): remove "drop_this_function" by sec_name after this is
    // fixed in llvm upstream.
    bool drop_this_function = false;
    StringRef dropped_function_section_name;
    for (llvm::object::SymbolRef &sr : iter->second) {
      func_aliases.push_back(llvm::cantFail(sr.getName()));
      StringRef sec_name =
          llvm::cantFail(llvm::cantFail(sr.getSection())->getName());
      if (!sec_name.startswith(".text")) {
        drop_this_function = true;
        dropped_function_section_name = sec_name;
        break;
      }
    }
    if (drop_this_function) {
      LOG(WARNING) << "Skipped symbol in none '.text.*' section '"
                                 << dropped_function_section_name.str()
                                 << "': " << func_aliases.front().str();
      continue;
    }
    CHECK(!func_aliases.empty());
    const uint64_t func_size =
        llvm::object::ELFSymbolRef(iter->second.front()).getSize();
    const auto &blocks = entry.value().bb_info;
    // TODO(b/183514655): revisit this after bug fixes.
    if (blocks.empty() || (blocks.size() == 1 && blocks.front().size == 0)) {
      LOG(WARNING) << "Skipped trivial bbentry : " << std::showbase << std::hex
                   << func_address;
      continue;
    }
    if (check_conflicting_symbols(func_size, blocks.size(), iter->second))
      continue;
    SymbolEntry *func_symbol =
        CreateFuncSymbolEntry(++ordinal, func_aliases.front(), func_aliases,
                              blocks.size(), func_address, func_size);
    // Note - func_symbol can be null, skip creating basic block symbols.
    if (func_symbol == nullptr) continue;
    // TODO(shenhan): refactoring out the following loop into
    // "CreateBbSymbolEntries" to avoid passing around multiple parameters to
    // "CreateSymbolEntry" function.
    for (int i = 0; i < blocks.size(); ++i) {
      // In rare cases when we fail to create a BbSymbolEntry, we bail out
      // because we don't want a in-complete representation of a function.
      if (!CreateBbSymbolEntry(++ordinal, func_symbol, i,
                               func_symbol->addr + blocks[i].offset,
                               blocks[i].size, blocks[i].meta))
        return false;
    }
  }  // end of iterating bb info section.
  remove_conflicting_symbols();
  return true;
}

bool PropellerWholeProgramInfo::PopulateSymbolMap() {
  ReadSymbolTable();
  bool t = ReadBbAddrMapSection();
  for (const auto &pa : bb_addr_map_) {
    CalculateStaticCfgInfo(pa.second);
  }
  return true;
}

bool PropellerWholeProgramInfo::PopulateSymbolMapWithPromise(
    std::promise<bool> result_promise) {
  bool result = PopulateSymbolMap();
  result_promise.set_value_at_thread_exit(result);
  return result;
}

// Parse perf data file.
Optional<LBRAggregation> PropellerWholeProgramInfo::ParsePerfData() {
  if (!options_.perf_names_size()) return llvm::None;
  std::string match_mmap_name = options_.binary_name();
  if (options_.has_profiled_binary_name())
    // If user specified "--profiled_binary_name", we use it.
    match_mmap_name = options_.profiled_binary_name();
  else if (!options_.ignore_build_id())
    // Set match_mmap_name to "", so PerfDataReader::SelectPerfInfo auto
    // picks filename based on build-id, if build id is present; otherwise,
    // PerfDataReader::SelectPerfInfo uses options_.binary_name to match mmap
    // event file name.
    match_mmap_name = "";

  LBRAggregation lbr_aggregation;

  int fi = 0;
  binary_perf_info_.ResetPerfInfo();
  for (const std::string &perf_file :  options_.perf_names()) {
    if (perf_file.empty()) continue;
    LOG(INFO) << "Parsing '" << perf_file << "' [" << ++fi << " of "
              << options_.perf_names_size() << "] ...";
    if (!PerfDataReader().SelectPerfInfo(perf_file, match_mmap_name,
                                         &binary_perf_info_)) {
      LOG(WARNING) << "Skipped profile '" << perf_file
                   << "', because reading file failed or no mmap found.";
      continue;
    }
    if (binary_perf_info_.binary_mmaps.empty()) {
      LOG(WARNING) << "Skipped profile '" << perf_file
                   << "', because no matching mmap found.";
      continue;
    }
    stats_.binary_mmap_num += binary_perf_info_.binary_mmaps.size();
    ++stats_.perf_file_parsed;
    perf_data_reader_.AggregateLBR(binary_perf_info_, &lbr_aggregation);
    if (!options_.keep_frontend_intermediate_data()) {
      // "keep_frontend_intermediate_data" is only used by tests.
      binary_perf_info_.ResetPerfInfo();  // Release quipper parser memory.
    } else if (options_.perf_names_size() > 1) {
      // If there are multiple perf data files, we must always call
      // ResetPerfInfo regardless of options_.keep_frontend_intermediate_data.
      LOG(ERROR) << "Usage error: --keep_frontend_intermediate_data is only "
                    "valid for single profile file input.";
      return llvm::None;
    }
  }
  stats_.br_counters_accumulated += std::accumulate(
      lbr_aggregation.branch_counters.begin(),
      lbr_aggregation.branch_counters.end(), 0,
      [](uint64_t cnt, typename BranchCountersTy::value_type &v) {
        return cnt + v.second;
      });
  if (stats_.br_counters_accumulated <= 100)
    LOG(WARNING) << "Too few branch records in perf data.";
  if (!stats_.perf_file_parsed) {
    LOG(ERROR) << "No perf file is parsed, cannot proceed.";
    return llvm::None;
  }
  return Optional<LBRAggregation>(std::move(lbr_aggregation));
}

bool PropellerWholeProgramInfo::DoCreateCfgs(LBRAggregation &&lbr_aggregation) {
  std::map<const SymbolEntry *, std::vector<SymbolEntry *>, SymbolPtrComparator>
      largest_alias_syms;
  for (auto &bb_group : bb_addr_map_) {
    const SymbolEntry *func_symbol = bb_group.second[0]->func_ptr;
    std::vector<SymbolEntry *> &syms = bb_group.second;
    auto [cur_iter, inserted] = largest_alias_syms.emplace(func_symbol, syms);
    if (!inserted && syms.size() > cur_iter->second.size()) {
      cur_iter->second = syms;
    }
  }

  // Temp map from SymbolEntry -> CFGNode.
  std::map<const SymbolEntry *, CFGNode *, SymbolPtrComparator> tmp_node_map;
  // Temp map from SymbolEntry -> ControlFlowGraph.
  std::map<const SymbolEntry *, std::unique_ptr<ControlFlowGraph>,
           SymbolPtrComparator>
      tmp_cfg_map;
  for (auto& [func_symbol, syms] : largest_alias_syms) {
    CHECK(!syms.empty());
    CHECK(func_symbol);
    CHECK(tmp_cfg_map.find(func_symbol) == tmp_cfg_map.end());

    std::sort(syms.begin(), syms.end(), SymbolPtrComparator());
    auto cfg = std::make_unique<ControlFlowGraph>(func_symbol->aliases);

    cfg->CreateNodes(syms);
    stats_.nodes_created += cfg->nodes().size();
    // Setup mapping from syms <-> nodes.
    DCHECK_EQ(cfg->nodes().size(), syms.size());
    std::vector<SymbolEntry *>::iterator symsi = syms.begin();
    for (const auto &nptr : cfg->nodes()) {
      CHECK((*symsi)->ordinal == nptr.get()->symbol_ordinal());
      tmp_node_map[*(symsi++)] = nptr.get();
    }
    tmp_cfg_map.emplace(func_symbol, std::move(cfg));
    ++stats_.cfgs_created;
  }
  if (!CreateEdges(lbr_aggregation, tmp_node_map))
    return false;

  for (auto &cfgi : tmp_cfg_map) {
    std::unique_ptr<ControlFlowGraph> cfg_ptr = std::move(cfgi.second);
    cfg_ptr->FinishCreatingControlFlowGraph();
    cfgs_.emplace(std::piecewise_construct,
                  std::forward_as_tuple(cfgi.first->name),
                  std::forward_as_tuple(std::move(cfg_ptr)));
  }

  // Release / cleanup.
  if (!options_.keep_frontend_intermediate_data()) {
    binary_perf_info_.binary_mmaps.clear();
    lbr_aggregation = LBRAggregation();
    // Release ownership and delete SymbolEntry instances.
    address_map_.clear();
    symtab_.clear();
    bb_addr_map_.clear();
  }

  return true;
}

CFGEdge *PropellerWholeProgramInfo::InternalCreateEdge(
    const SymbolEntry *from_sym, const SymbolEntry *to_sym, uint64_t weight,
    CFGEdge::Kind edge_kind,
    const std::map<const SymbolEntry *, CFGNode *, SymbolPtrComparator>
        &tmp_node_map,
    std::map<SymbolPtrPair, CFGEdge *, SymbolPtrPairComparator> *tmp_edge_map) {
  CFGEdge *edge = nullptr;
  auto i = tmp_edge_map->find(std::make_pair(from_sym, to_sym));
  if (i != tmp_edge_map->end()) {
    edge = i->second;
    if (edge->kind() != edge_kind) {
      LOG(WARNING) << "Edges with same src and sink have different type: "
                   << CFGEdgeNameFormatter(edge) << " has type " << edge_kind
                   << " and " << edge->kind();
      stats_.edges_with_same_src_sink_but_different_type++;
    }
    edge->IncrementWeight(weight);
  } else {
    auto from_ni = tmp_node_map.find(from_sym),
         to_ni = tmp_node_map.find(to_sym);
    if (from_ni == tmp_node_map.end() || to_ni == tmp_node_map.end())
      return nullptr;
    CFGNode *from_node = from_ni->second;
    CFGNode *to_node = to_ni->second;
    DCHECK(from_node && to_node);
    edge = from_node->cfg()->CreateEdge(from_node, to_node, weight, edge_kind);
    ++stats_.edges_created;
    tmp_edge_map->emplace(std::piecewise_construct,
                          std::forward_as_tuple(from_sym, to_sym),
                          std::forward_as_tuple(edge));
  }
  return edge;
}

// Create control flow graph edges from branch_counters_. For each address pair
// <from_addr, to_addr> in "branch_counters_", we translate it to <from_symbol,
// to_symbol> and by using tmp_node_map, we further translate it to <from_node,
// to_node>, and finally create a CFGEdge for such CFGNode pair.
bool PropellerWholeProgramInfo::CreateEdges(
    const LBRAggregation &lbr_aggregation,
    const std::map<const SymbolEntry *, CFGNode *, SymbolPtrComparator>
        &tmp_node_map) {
  // Temp map that records which CFGEdges are created, so we do not re-create
  // edges. Note this is necessary: although
  // "branch_counters_" have no duplicated <from_addr, to_addr> pairs, the
  // translated <from_sym, to_sym> may have duplicates.
  std::map<SymbolPtrPair, CFGEdge *, SymbolPtrPairComparator> tmp_edge_map;

  std::map<SymbolPtrPair, uint64_t, SymbolPtrPairComparator>
      tmp_bb_fallthrough_counters;

  uint64_t weight_on_dubious_edges = 0;
  uint64_t total_weight_created = 0;
  uint64_t edges_recorded = 0;
  for (const typename BranchCountersTy::value_type &bcnt :
       lbr_aggregation.branch_counters) {
    ++edges_recorded;
    uint64_t from = bcnt.first.first;
    uint64_t to = bcnt.first.second;
    uint64_t weight = bcnt.second;
    const SymbolEntry *from_sym = FindSymbolUsingBinaryAddress(from);
    const SymbolEntry *to_sym = FindSymbolUsingBinaryAddress(to);
    if (!from_sym || !to_sym) continue;

    if (!from_sym->IsReturnBlock() &&
        ((to_sym->IsBasicBlock() && to_sym->addr != to) ||
         (!to_sym->IsBasicBlock() && to_sym->func_ptr->addr != to))) {
      // Jump is not a return and its target is not the beginning of a
      // function or a basic block
      weight_on_dubious_edges += weight;
    }
    total_weight_created += weight;

    // TODO(b/162192070): for bbaddrmap workflow, enable the following:
    // CHECK(from_sym->IsBasicBlock());
    // CHECK(to_sym->IsBasicBlock());

    // This is to handle the case when a call is the last instr of a basicblock,
    // and a return to the beginning of the next basic block, change the to_sym
    // to the basic block just before and add fallthrough between the two
    // symbols. After this code executes, "to" can never be the beginning
    // of a basic block for returns.
    if ((from_sym->IsReturnBlock() || to_sym->func_ptr != from_sym->func_ptr) &&
        to_sym->func_ptr->addr != to &&  // Not a call
        // Jump to the beginning of the basicblock
        to == to_sym->addr) {
      auto to_address_iter = address_map_.find(to);
      if (to_address_iter != address_map_.end() &&
          to_address_iter != address_map_.begin()) {
        auto prev_iter = std::prev(to_address_iter);
        if (!prev_iter->second.empty()) {
          const SymbolEntry *callsite_sym = prev_iter->second.back().get();
          if (callsite_sym) {
            // Account for the fall-through between callSiteSym and toSym.
            tmp_bb_fallthrough_counters[std::make_pair(callsite_sym, to_sym)] +=
                weight;
            // Reassign toSym to be the actuall callsite symbol entry.
            to_sym = callsite_sym;
          } else {
            LOG(WARNING) << "*** Warning: Could not find the right "
                            "call site sym for : "
                         << to_sym->name.str();
          }
        }
      }
    }

    CFGEdge::Kind edge_kind = CFGEdge::Kind::kBranchOrFallthough;
    if (to_sym->func_ptr->addr == to)
      edge_kind = CFGEdge::Kind::kCall;
    else if (to != to_sym->addr || from_sym->IsReturnBlock())
      edge_kind = CFGEdge::Kind::kRet;

    InternalCreateEdge(from_sym, to_sym, weight, edge_kind, tmp_node_map,
                       &tmp_edge_map);
  }

  if (weight_on_dubious_edges / static_cast<double>(total_weight_created) >
      0.3) {
    LOG(ERROR) << "Too many jumps into middle of basic blocks detected, "
                  "probably because of source drift ("
               << CommaStyleNumberFormatter(weight_on_dubious_edges)
               << " out of " << CommaStyleNumberFormatter(total_weight_created)
               << ").";
    return false;
  }

  if (stats_.edges_created / static_cast<double>(edges_recorded) < 0.0005) {
    LOG(ERROR)
        << "Fewer than 0.05% recorded jumps are converted into CFG edges, "
           "probably because of source drift.";
    return false;
  }

  CreateFallthroughs(lbr_aggregation, tmp_node_map,
                     &tmp_bb_fallthrough_counters, &tmp_edge_map);
  return true;
}

// Create edges for fallthroughs.
// 1. translate fallthrough pairs "<from, to>"s -> "<from_sym, to_sym>"s.
// 2. calculate all symbols between "from_sym" and "to_sym", so we get the
//    symbol path: <from_sym, internal_sym1, internal_sym2, ... , internal_symn,
//    to_sym>.
// 3. create edges and apply weights for the above path.
void PropellerWholeProgramInfo::CreateFallthroughs(
    const LBRAggregation &lbr_aggregation,
    const std::map<const SymbolEntry *, CFGNode *, SymbolPtrComparator>
        &tmp_node_map,
    std::map<SymbolPtrPair, uint64_t, SymbolPtrPairComparator>
        *tmp_bb_fallthrough_counters,
    std::map<SymbolPtrPair, CFGEdge *, SymbolPtrPairComparator> *tmp_edge_map) {
  for (auto &i : lbr_aggregation.fallthrough_counters) {
    uint64_t cnt = i.second;
    auto *from_sym = FindSymbolUsingBinaryAddress(i.first.first);
    auto *to_sym = FindSymbolUsingBinaryAddress(i.first.second);
    if (from_sym && to_sym)
      (*tmp_bb_fallthrough_counters)[std::make_pair(from_sym, to_sym)] += cnt;
  }

  for (auto &i : *tmp_bb_fallthrough_counters) {
    std::vector<const SymbolEntry *> path;
    const SymbolEntry *fallthrough_from = i.first.first,
                      *fallthrough_to = i.first.second;
    uint64_t weight = i.second;
    if (fallthrough_from == fallthrough_to ||
        !CalculateFallthroughBBs(*fallthrough_from, *fallthrough_to, &path))
      continue;

    path.push_back(fallthrough_to);
    auto *from_sym = fallthrough_from;
    CHECK_NE(from_sym, nullptr);
    for (auto *to_sym : path) {
      CHECK_NE(to_sym, nullptr);
      auto *fallthrough_edge = InternalCreateEdge(
          from_sym, to_sym, weight, CFGEdge::Kind::kBranchOrFallthough,
          tmp_node_map, tmp_edge_map);
      if (!fallthrough_edge) break;
      from_sym = to_sym;
    }
  }
}

// Calculate fallthrough basicblocks between <from_sym, to_sym>. All symbols
// that situate between the end of from_sym and the beginning of to_sym are
// put into "path". Note: all symbols involved here belong to the same
// function, because there are no fallthroughs across function boundary.
bool PropellerWholeProgramInfo::CalculateFallthroughBBs(
    const SymbolEntry &from, const SymbolEntry &to,
    std::vector<const SymbolEntry *> *path) {
  path->clear();
  if (from == to) return true;
  if (from.addr > to.addr) {
    LOG(WARNING) << "*** Internal error: fallthrough path start address is "
                  "larger than end address. ***";
    return false;
  }
  if (!from.CanFallThrough()) {
    LOG(WARNING) << "*** Skipping non-fallthrough ***"
                 << SymbolNameFormatter(from);
    return false;
  }
  auto p = address_map_.find(from.addr), q = address_map_.find(to.addr),
       e = address_map_.end();
  if (p == e || q == e) {
    LOG(FATAL) << "*** Internal error: invalid symbol in fallthrough pair. ***";
    return false;
  }
  q = std::next(q);
  if (from.func_ptr != to.func_ptr) {
    LOG(ERROR)
        << "fallthrough (" << SymbolNameFormatter(from) << " -> "
        << SymbolNameFormatter(to)
        << ") does not start and end within the same function.";
    return false;
  }
  auto func = from.func_ptr;
  auto i = p;
  for (++i; i != q && i != e; ++i) {
    if (i->second.empty()) continue;
    if (i->second.size() == 1) {  // 99% of the cases.
      SymbolEntry *s1 = i->second.front().get();
      if (*s1 == to)
        break;
      // (b/62827958) Sometimes LBR contains duplicate entries in the beginning
      // of the stack which may result in false fallthrough paths. We discard
      // the fallthrough path if any intermediate block (except the destination
      // block) does not fall through (source block is checked before entering
      // this loop).
      if (!s1->CanFallThrough()) {
        LOG(WARNING) << "*** Skipping non-fallthrough ***"
                 << SymbolNameFormatter(*s1);
        return false;
      }
      if (s1->func_ptr == func) {
        path->push_back(s1);
        continue;
      } else {
        LOG(ERROR) << "Found symbol " << SymbolNameFormatter(s1)
                   << " in the path of (" << SymbolNameFormatter(from) << " -> "
                   << SymbolNameFormatter(to)
                   << ") that is in a different function.";
        return false;
      }
    }
    // Handling multiple symbols with the same address.
    // One particular case is while calculating fallthroughs for (71.BB.foo
    // -> 73.BB.foo) while 73.BB.foo and 72.BB.foo have the same address, like
    // below:
    //     0x10 71.BB.foo
    //     0x15 xx.BB.foo
    //     0x20 72.BB.foo 73.BB.foo
    // In this case, we shall only include xx.BB.foo and 72.BB.foo, but
    // leave 73.BB.foo alone.

    // TODO(b/154263650, rahmanl): only include symbols that meets
    // SymbolEntry::isFallThroughBlock(). This information is accessible only
    // after we have bbinfo implemented.
    std::vector<SymbolEntry *> candidates;
    uint64_t last_ordinal = 0;
    for (auto &se : i->second) {
      CHECK(last_ordinal == 0 || last_ordinal < se->ordinal);
      last_ordinal = se->ordinal;
      if (*se != to)
        candidates.push_back(se.get());
    }
    for (auto *sptr : candidates) CHECK(sptr->ordinal);
    std::sort(candidates.begin(), candidates.end(), SymbolPtrComparator());
    // find the first that is not from the same function
    std::vector<SymbolEntry *>::iterator i1 = candidates.begin(),
                                         ie = candidates.end();
    while (i1 != ie && (*i1)->IsBasicBlock() && (*i1)->func_ptr == func) ++i1;
    if (i1 != ie) candidates.erase(i1, ie);
    if (candidates.empty()) {
      LOG(ERROR) << "failed to find a BB for "
                 << "fallthrough (" << SymbolNameFormatter(from) << " -> "
                 << SymbolNameFormatter(to) << ").";
      return false;
    }
    path->insert(path->end(), candidates.begin(), candidates.end());
  }
  if (path->size() >= 200)
    LOG(WARNING)
        << "More than 200 BBs along fallthrough (" << SymbolNameFormatter(from)
        << " -> " << SymbolNameFormatter(to) << "): " << std::dec
        << path->size() << " BBs.";
  return true;
}

bool PropellerWholeProgramInfo::CreateCfgs() {
  std::promise<bool> read_symbols_promise;
  std::future<bool> future_result = read_symbols_promise.get_future();
  std::thread populate_symbol_map_thread(
      &PropellerWholeProgramInfo::PopulateSymbolMapWithPromise, this,
      std::move(read_symbols_promise));

  // Optional<LBRAggregation> opt_lbr_aggregation = ParsePerfData();
  populate_symbol_map_thread.join();
  // if (!future_result.get() || !opt_lbr_aggregation) return false;
  return true;

  // This must be done only after both PopulateSymbolMaps and ParsePerfData
  // finish. Note: opt_lbr_aggregation is released afterwards.
  // return DoCreateCfgs(std::move(*opt_lbr_aggregation));
}
}  // namespace devtools_crosstool_autofdo
