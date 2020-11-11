#include "llvm_propeller_whole_program_info.h"

#include <fcntl.h>  // for "O_RDONLY"

#include <cstdint>
#include <future>  // NOLINT(build/c++11)
#include <thread>  // NOLINT(build/c++11)
#include <tuple>
#include <type_traits>
#include <utility>

#include "llvm_propeller_formatting.h"
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

// Section .bb_addr_map consists of many AddrMapEntry.
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
}  // namespace

// Return true if func_sym is a valid wrapping function for bbName.
// For example, obj@0x12345 {name=foo, alias={foo, foo1, foo2}} is a valid
// wrapping function symbol for bbName "aaaa.BB.foo2".
static bool
IsFunctionForBBName(SymbolEntry *func_sym, llvm::StringRef bb_name) {
  auto a = bb_name.split(kBasicBlockSeparator);
  if (a.second == func_sym->name)
    return true;
  for (auto n : func_sym->aliases)
    if (a.second == n)
      return true;
  return false;
}

static bool
ContainsAnotherSymbol(SymbolEntry *sym, SymbolEntry *other) {
  // Note if other's size is 0, we allow O on the end boundary. For example, if
  // foo.BB.4 is at address 0x10. foo is [0x0, 0x10), we then assume foo
  // contains foo.BB.4.
  return sym->addr <= other->addr &&
         (other->addr + other->size) < (sym->addr + sym->size + 1);
}

static bool
IsValidWrappingFunction(SymbolEntry *func_sym, SymbolEntry *bb_sym) {
  return IsFunctionForBBName(func_sym, bb_sym->name) &&
         ContainsAnotherSymbol(func_sym, bb_sym);
}

static llvm::Optional<llvm::object::SectionRef> FindBbAddrMapSection(
    const llvm::object::ObjectFile &obj) {
  for (auto sec : obj.sections()) {
    Expected<llvm::StringRef> sn = sec.getName();
    llvm::object::ELFSectionRef esec(sec);
#if LLVM_VERSION_MAJOR >= 12
    if (esec.getType() == llvm::ELF::SHT_LLVM_BB_ADDR_MAP &&
        sn && (*sn) == PropellerWholeProgramInfo::kBbAddrMapSectionName)
#else
    if (sn && (*sn) == PropellerWholeProgramInfo::kBbAddrMapSectionName)
#endif
      return sec;
  }
  return llvm::None;
}

std::unique_ptr<PropellerWholeProgramInfo> PropellerWholeProgramInfo::Create(
    const PropellerOptions &options) {
  BinaryPerfInfo bpi;
  if (!PerfDataReader().SelectBinaryInfo(options.binary_name, &bpi.binary_info))
    return nullptr;

  Optional<llvm::object::SectionRef> opt_bb_addr_map_section =
      FindBbAddrMapSection(*(bpi.binary_info.object_file));
  // Do not use std::make_unique, ctor is not public.
  if (opt_bb_addr_map_section)
    LOG(INFO) << "'" << options.binary_name << "' has '"
              << kBbAddrMapSectionName << "' section.";
  return std::unique_ptr<PropellerWholeProgramInfo>(
      new PropellerWholeProgramInfo(options, std::move(bpi),
                                    std::move(opt_bb_addr_map_section)));
}

// Emplace symbols to name_map_.
//
// For function symbol obj@0x12345 {name=foo, alias={foo, foo1, foo2}}, this is
// emplaced like:
//   name_map_["foo"][""] == 0x12345
//   name_map_["foo1"][""] == 0x12345
//   name_map_["foo2"][""] == 0x12345
//
// For bb symbol obj@0x6789a {name=aaaa.BB.foo1, func_ptr=0x12345}, this is
// emplaced like:
//   name_map_["foo1"]["aaaa"] == 0x6789a
bool PropellerWholeProgramInfo::EmplaceSymbol(SymbolEntry *sym) {
  CHECK(sym && sym->func_ptr);
  if (sym->IsFunction()) {
    bool duplicates_found = false;
    for (StringRef name : sym->aliases) {
      auto existing_fi = name_map_[name].find("");
      if (existing_fi != name_map_[name].end()) {
        existing_fi->second = nullptr;
        duplicates_found = true;
        LOG(WARNING) << "Duplicated entry: name_map_ slot[" << name.str()
                     << "][] already occupied by other symbol.";
        ++stats_.duplicate_symbols;
      } else {
        name_map_[name][""] = sym;
      }
    }
    return !duplicates_found;
  }
  auto r = sym->name.split(kBasicBlockSeparator);
  StringRef l1Index = r.second;
  StringRef l2Index = r.first;
  if (!name_map_[l1Index].emplace(l2Index, sym).second) {
    // b/159167176 : in case of duplicate symbols, give a warning and mark
    // such duplicates with nullptr. We shall not immediate "delete" the
    // SymbolEntry instances now, because we are still in the middle of setting
    // up name_map_ and address_map_.
    LOG(WARNING) << "Duplicated entry: name_map_ slot[" << l1Index.str() << "]["
                 << l2Index.str() << "] already occupied by other symbol.";
    name_map_[l1Index][l2Index] = nullptr;
    ++stats_.duplicate_symbols;
    return false;
  }
  return true;
}

// Minimal check if 2 names are ctor/dtor variants.
// <ctor-dtor-name> ::= C1      # complete object constructor
//                  ::= C2      # base object constructor
//                  ::= C3      # complete object allocating constructor
//                  ::= D0      # deleting destructor
//                  ::= D1      # complete object destructor
//                  ::= D2      # base object destructor
// Refer to:
// Itanium C++ ABI (yes, indeed, 2 "http://"s in the url):
//    http://web.archive.org/web/20100315072857/http://www.codesourcery.com/public/cxx-abi/abi.html
// _Z     | N      | 5Thing  | C1          | E          | i
// prefix | nested | `Thing` | Constructor | end nested | parameters: `int`
// _Z     | N      | 5Thing  | D1          | E          | v
// prefix | nested | `Thing` | Destructor  | end nested | void
static bool IsCtorAndDtorNameVariants(SymbolEntry *func_sym,
                                      SymbolEntry *bb_sym) {
  // If only 1 difference between 2 names
  StringRef n1 = func_sym->name;
  StringRef n2 = bb_sym->name.split(kBasicBlockSeparator).second;
  if (n1.size() != n2.size()) return false;
  auto size = n1.size();
  const unsigned char *s = n1.bytes_begin(), *t = n2.bytes_begin();
  int diff_pos = -1;
  for (int p = 0; p < size; ++p)
    if (s[p] != t[p]) {
      if (diff_pos == -1)
        diff_pos = p;
      else
        return false;
    }
  // Should  not happen, but for completeness.
  if (diff_pos == -1) return true;
  return diff_pos > 1 && diff_pos < size - 1 &&
         ((s[diff_pos - 1] == 'C' || s[diff_pos - 1] == 'D') &&
          s[diff_pos - 1] == t[diff_pos - 1]) &&
         (s[diff_pos + 1] == 'E' && t[diff_pos + 1] == 'E') &&
         '0' <= s[diff_pos] && s[diff_pos] <= '3' && '0' <= t[diff_pos] &&
         t[diff_pos] <= '3';
}

// Populate internal mapping "name_map_" and associate bbsymbols to their
// corresponding function symbols.
bool PropellerWholeProgramInfo::MapFunctionAndBasicBlockSymbols() {
  SymbolEntry *current_func = nullptr;
  for (auto p = address_map_.begin(); p != address_map_.end();
       (p->second.empty() ? p = address_map_.erase(p) : ++p)) {
    uint64_t address = p->first;
    if (p->second.empty()) continue;
    if (p->second.size() == 1) {  // 99% of the cases
      SymbolEntry *se = p->second.front().get();
      // Do not use se->IsBasicBlock() - because at this time, "se" is in
      // incomplete status, func_ptr is not set up yet.
      if (SymbolEntry::IsBbSymbol(se->name)) {
        if (current_func && IsValidWrappingFunction(current_func, se)) {
          se->func_ptr = current_func;
        } else {
          // In the following scenario, we explicitly check if symbol at
          // 0x314f0cb could be wrapped by symbol at 0x314f0b0.

          // 314f05c t a.BB._ZN4llvm19DependenceGraphInfoINS_7DDGNodeEEC2EOS2_
          // 314f068 t ra.BB._ZN4llvm19DependenceGraphInfoINS_7DDGNodeEEC2EOS2_
          // 314f0a1 t ara.BB._ZN4llvm19DependenceGraphInfoINS_7DDGNodeEEC2EOS2_
          // 314f0b0 W _ZN4llvm19DependenceGraphInfoINS_7DDGNodeEED1Ev
          // 314f0cb t r.BB._ZN4llvm19DependenceGraphInfoINS_7DDGNodeEED2Ev ***
          // 314f0d0 W _ZNK4llvm19DependenceGraphInfoINS_7DDGNodeEE7getNameEv
          // 314f0e0 W _ZNK4llvm19DependenceGraphInfoINS_7DDGNodeEE7getRootEv
          if (!IsCtorAndDtorNameVariants(current_func, se)) {
            LOG(WARNING) << "Drop symbol at address " << std::showbase
                         << std::hex << address << ", bb symbol '"
                         << se->name.str()
                         << "' does not have a valid function.";
            p->second.clear();
            ++stats_.dropped_symbols;
            continue;
          }
          // Note all names are StringRefs into readonly MemoryBuffer. So we do
          // renaming by copying.
          char *new_name = new char[se->name.size() + 1];
          int t = se->name.split(kBasicBlockSeparator).first.size() +
                  strlen(kBasicBlockSeparator);
          strncpy(new_name, se->name.data(), t);
          strncpy(new_name + t, current_func->name.data(),
                  current_func->name.size());
          // Note: no '\0' at the end of current_func->name. So we put one at
          // the end.
          new_name[se->name.size()] = '\0';
          StringRef old_name = se->name;
          se->name = StringRef(new_name);
          se->func_ptr = current_func;
          CHECK(IsFunctionForBBName(current_func, se->name));
          string_vault_.emplace_back(new_name);
          LOG(INFO) << "Renamed '" << old_name.str() << "' to '" << new_name
                    << "'.";
        }
      } else {
        se->func_ptr = se;
        current_func = se;
      }
      se->ordinal = AllocateSymbolOrdinal();
      EmplaceSymbol(se);
      continue;
    }

    if (p->second.size() > 2) {
      // We have to drop symbols like below:
      //  0000000003725e0f 5b t aa.foo
      //  0000000003725e0f 0  t aaa.foo
      //  0000000003725e0f 0  t aaaa.foo
      LOG(WARNING)
          << "Drop all symbols at address " << std::showbase << std::hex
          << address
          << ": cannot process more than 2 bb/func symbols at the same "
             "address.";
      stats_.dropped_symbols += p->second.size();
      p->second.clear();
      continue;
    }

    //  Now dealing with 2 symbols on the same address.
    bool all_are_bbs = true;
    SymbolEntry *func_sym = nullptr;
    for (auto &s : p->second) {
      bool s_is_bb = SymbolEntry::IsBbSymbol(s->name);
      all_are_bbs &= s_is_bb;
      if (!s_is_bb) {
        if (func_sym) {
          // Case 0: 2 func symbols, error and return.
          LOG(ERROR) << "Analyzing failure: at address " << std::showbase
                     << std::hex << address
                     << ", more than 1 function symbols defined that cannot be "
                        "aliased.";
          return false;
        }
        func_sym = s.get();
      }
    }

    // Case 1: both are bb symbols.
    if (all_are_bbs) {
      if (!current_func) {
        LOG(ERROR) << "Analyzing failure: at address " << std::showbase
                   << std::hex << address
                   << ", symbols do not have a containing func.";
        return false;
      }
      auto *front = p->second.front().get();
      auto *back = p->second.back().get();
      if (IsFunctionForBBName(current_func, front->name) &&
          IsFunctionForBBName(current_func, back->name)) {
        front->func_ptr = current_func;
        back->func_ptr = current_func;
      } else {
        LOG(WARNING) << "Drop all bb symbols at address " << std::showbase
                     << std::hex << address
                     << ": cannot find a wrapping function for both.";
        p->second.clear();
        continue;
      }
      if (front->size != 0 && back->size != 0)
        LOG(WARNING) << "2 non-empty bb symbols at address "
                   << std::showbase << std::hex << address;
      if (front->size == 0 && back->size == 0)
        LOG(WARNING) << "2 empty bb symbols at address " << std::showbase
                     << std::hex << address;
      // Zero sized bb is placed before the non zero sized bb.
      if (front->size != 0) p->second.front().swap(p->second.back());
      p->second.front()->ordinal = AllocateSymbolOrdinal();
      p->second.back()->ordinal = AllocateSymbolOrdinal();
      EmplaceSymbol(p->second.front().get());
      EmplaceSymbol(p->second.back().get());
      continue;
    }

    // Case 2: 1 func sym, 1 bb sym.
    func_sym->func_ptr = func_sym;
    SymbolEntry *bb_sym = (p->second.front().get() == func_sym)
                             ? p->second.back().get()
                             : p->second.front().get();
    if (!SymbolEntry::IsBbSymbol(bb_sym->name) ||
        SymbolEntry::IsBbSymbol(func_sym->name)) {
      LOG(ERROR) << "Analyzing failure: unexpected mixed symbol at "
                 << std::showbase << std::hex << address;
      return false;
    }

    // So now we have func_sym and bb_sym on the same address.
    if (IsFunctionForBBName(func_sym, bb_sym->name)) {
      // Case 2.1: bb_sym belongs to func_sym.
      // 00000000018a3047 000000000000000c t  aaa.BB.foo   <- last_sym
      // 00000000018a3060 000000000000000c t  a.BB.bar     <- bb_sym
      // 00000000018a3060 000000000000000d W  bar          <- func_sym
      bb_sym->func_ptr = func_sym;
      if (p->second.front().get() != func_sym)
        p->second.front().swap(p->second.back());
    } else {
      // Case 2.2: bb_sym bleongs to last_sym.
      // 00000000018a3047 000000000000000c t  aaa.BB.foo   <- last_sym
      // 00000000018a3060 000000000000000c t aaaa.BB.foo   <- bb_sym
      // 00000000018a3060 000000000000000d W  bar          <- func_sym
      SymbolEntry *last_sym = p == address_map_.begin()
                                 ? nullptr
                                 : (std::prev(p))->second.back().get();
      if (last_sym && IsFunctionForBBName(last_sym->func_ptr, bb_sym->name)) {
        bb_sym->func_ptr = last_sym->func_ptr;
        if (p->second.front().get() != bb_sym)
          p->second.front().swap(p->second.back());
      } else {
        LOG(ERROR) << "Unable to analyze " << std::showbase << std::hex
                   << address;
        return false;
      }
    }
    current_func = func_sym;
    p->second.front()->ordinal = AllocateSymbolOrdinal();
    p->second.back()->ordinal = AllocateSymbolOrdinal();
    EmplaceSymbol(p->second.front().get());
    EmplaceSymbol(p->second.back().get());
  }  // End of iterating address_map_
  return true;
}

// If we have the following symbol entry,
//  obj@0x12345 {symbolentry: name=f1, alias={f1, f2, f3}, func_ptr=0x12345}
//  obj@0x45678 {symbolentry: name=a.BB.f3, bbtag=true, funct_ptr=0x12345}
//  obj@0x789ab {symbolentry: name=aa.BB.f3, bbtag=true,funct_ptr=0x12345}
//  obj@0xabcde {symbolentry: name=aaa.BB.f3, bbtag=true, funct_ptr=0x12345}
// These objects are mapped into name_map_ like below:
//
//  {  "f1":
//          {
//             "": obj@0x12345  {symbolentry: name=f1, alias={f1, f2, f3}}
//          },
//     "f2":
//          {
//             "": obj@0x12345  {symbolentry: name=f1, alias={f1, f2, f3}}
//          },
//     "f3":
//          {
//             "": obj@0x12345  {symbolentry: name=f1, alias={f1, f2, f3}}
//             "a": obj@0x45678   {symbolentry: name=a.BB.f3, bbtag=true},
//             "aa": obj@0x789ab  {symbolentry: name=aa.BB.f3, bbtag=true},
//             "aaa": obj@0xabcde {symbolentry: name=aaa.BB.f3, bbtag=true},
//          }
//  }
//
// This FixupNameMap changes obj@0x12345 as well as the mapping into:
//  obj@0x12345 {symbolentry: name=f3, alias={f3, f1, f2}, func_ptr=0x12345}
//
//  {  "f1":
//          {
//             "": obj@0x12345  {symbolentry: name=f3, alias={f3, f1, f2}}
//          },
//     "f2":
//          {
//             "": obj@0x12345  {symbolentry: name=f3, alias={f3, f1, f2}}
//          },
//     "f3":
//          {
//             "": obj@0x12345  {symbolentry: name=f3, alias={f3, f1, f2}}
//             "a": obj@0x45678   {symbolentry: name=a.BB.f3, bbtag=true},
//             "aa": obj@0x789ab  {symbolentry: name=aa.BB.f3, bbtag=true},
//             "aaa": obj@0xabcde {symbolentry: name=aaa.BB.f3, bbtag=true},
//          }
//  }
bool PropellerWholeProgramInfo::FixupNameMap() {
  for (auto i = name_map_.begin(), e = name_map_.end(); i != e;
       i = (i->second.empty() ? name_map_.erase(i) : ++i)) {
    NameMapTy::mapped_type &function_bbs = i->second;
    // Prune duplicate symbols (duplicates are marked by nullptr in name_map_).
    bool duplicates_found = false;
    for (auto si = function_bbs.begin(), se = function_bbs.end();
         !duplicates_found && si != se; ++si)
      duplicates_found = (si->second == nullptr);
    if (duplicates_found)
      function_bbs.clear();
    if (function_bbs.empty()) continue;

    SymbolEntry *func_sym = function_bbs[""];
    if (!func_sym) {
      LOG(ERROR) << "Internal error. No func ptr for '" << i->first.str()
                 << "'.";
      return false;
    }
    StringRef bb_func_name = "";
    for (auto &j : function_bbs) {
      SymbolEntry *sym = j.second;
      if (sym->IsFunction()) continue;
      StringRef n = sym->name.split(kBasicBlockSeparator).second;
      if (!bb_func_name.empty() && bb_func_name != n) {
        LOG(ERROR) << "Internal error. name_map_['" << i->first.str()
                   << "'] contains bbs that have different func name part.";
        return false;
      }
      bb_func_name = n;
    }
    if (!bb_func_name.empty() && bb_func_name != func_sym->name &&
        !FixupFuncName(func_sym, bb_func_name))
      return false;
    // We only shorten bb name after FixupFuncName is done.
    for (auto &j : function_bbs)
      if (j.second->IsBasicBlock()) ShortenBBName(j.second);
  }
  return true;
}

// One tricky thing to fix:
//    Func symbol: name=_zfooc2, aliases={_zfooc2, _zfooc1, _zfooc3}
//    BB symbol: a.BB._zfooc1
//
// We want to make sure the primary name (the name first appears in the
// alias) matches the bb name, so we change the func's name and aliases to:
//    name=_zfooc1, aliases={_zfooc1, _zfooc2, _zfooc3}
bool PropellerWholeProgramInfo::FixupFuncName(SymbolEntry *func_sym,
                                              StringRef bb_func_name) {
  if (func_sym->aliases.size() > 1) {
    if (bb_func_name != func_sym->name) {
      auto &aliases = func_sym->aliases;
      SymbolEntry::AliasesTy::iterator p, q;
      for (p = aliases.begin(), q = aliases.end(); p != q; ++p)
        if (*p == bb_func_name) break;

      if (p == q) {
        LOG(ERROR) << "Internal check error: bb symbol '" << bb_func_name.str()
                   << "' does not have a valid wrapping function.";
        return false;
      }
      if (p != aliases.begin()) {
        func_sym->name = bb_func_name;
        aliases.erase(p);
        aliases.insert(aliases.begin(), bb_func_name);
      }
    }
  }
  return true;
}

// Shorten bb name "aaa.BB.main" to "aaa".
bool PropellerWholeProgramInfo::ShortenBBName(SymbolEntry *bb_sym) {
  StringRef fname, bname;
  bool r = SymbolEntry::IsBbSymbol(bb_sym->name, &fname, &bname);
  if (!r || fname != bb_sym->func_ptr->name) {
    LOG(ERROR) << "Internal check error: bb symbol '" << bb_sym->name.str()
               << "' in conflict state.";
    return false;
  }
  bb_sym->name = bname;
  return true;
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
  SymbolEntry *funcsym =
      new SymbolEntry(ordinal, func_name, aliases, address, size, nullptr, 0);
  // Put into address_map_.
  address_map_[address].emplace_back(funcsym);
  ++stats_.syms_created;
  return funcsym;
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
  SymbolEntry *bb_symbol =
      new SymbolEntry(ordinal, "", SymbolEntry::AliasesTy(), address, size,
                      parent_func, metadata);
  CHECK(bb_index < iter->second.size());
  iter->second[bb_index] = bb_symbol;
  // Put into address_map_.
  address_map_[address].emplace_back(bb_symbol);
  ++stats_.syms_created;
  return bb_symbol;
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

    auto &addr_sym_list = symtab_[*address];
    // Check whether there are already symbols on the same address, if so make
    // sure they have the same size and thus they can be aliased.
    const uint64_t func_size = symbol.getSize();
    bool check_size_ok = true;
    for (auto &sym_ref : addr_sym_list) {
      uint64_t sym_size = llvm::object::ELFSymbolRef(sym_ref).getSize();
      if (func_size != sym_size) {
        LOG(WARNING) << "Multiple function symbols on the same address with "
                        "different size: "
                     << AddressFormatter(*address) << ": '" << func_name->str()
                     << "(" << func_size << ")' and '"
                     << llvm::cantFail(sym_ref.getName()).str() << "("
                     << sym_size << ")', the latter will be dropped.";
        check_size_ok = false;
        break;
      }
    }
    if (check_size_ok) addr_sym_list.push_back(sr);
  }
}

bool PropellerWholeProgramInfo::ReadBbAddrMapSection() {
  llvm::Expected<StringRef> exp_contents =
      (*opt_bb_addr_map_section_).getContents();
  if (!exp_contents) {
    LOG(ERROR) << "Error accessing .bb_info section content.";
    return false;
  }
  StringRef sec_contents = *exp_contents;
  const unsigned char * p = sec_contents.bytes_begin();
  const unsigned char * const end = sec_contents.bytes_end();

  uint64_t ordinal = 0;
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
      LOG(WARNING) << "Invalid entry inside '.bb_addr_map', at offset: "
                   << std::showbase << std::hex
                   << p - sec_contents.bytes_begin()
                   << ". Function address listed: " << std::showbase << std::hex
                   << func_address;
      ++stats_.bbaddrmap_function_does_not_have_symtab_entry;
      continue;
    }
    SymbolEntry::AliasesTy func_aliases;
    for (llvm::object::SymbolRef &sr : iter->second) {
      func_aliases.push_back(llvm::cantFail(sr.getName()));
    }
    CHECK(!func_aliases.empty());
    const uint64_t func_size =
        llvm::object::ELFSymbolRef(iter->second.front()).getSize();
    const auto &blocks = entry.value().bb_info;
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
  return true;
}

// Populate internal data structure: name_map_ and address_map_ from bb_info
// section && binary symbol table.
bool PropellerWholeProgramInfo::PopulateSymbolMapFromBbAddrMap(
    std::promise<bool> result_promise) {
  ReadSymbolTable();
  bool result = ReadBbAddrMapSection();
  result_promise.set_value_at_thread_exit(result);
  return result;
}

// Populate internal data structure: name_map_ and address_map_.
bool PropellerWholeProgramInfo::PopulateSymbolMap() {
  auto symbols = binary_perf_info_.binary_info.object_file->symbols();
  for (const auto &sym : symbols) {
    auto addr_e = sym.getAddress();
    auto section_e = sym.getSection();
    auto name_e = sym.getName();
    auto type_e = sym.getType();
    if (!(addr_e && *addr_e && section_e && (*section_e)->isText() && name_e &&
          type_e))
      continue;
    StringRef name = *name_e;
    if (name.empty()) continue;
    uint64_t addr = *addr_e;
    uint8_t type(*type_e);
    llvm::object::ELFSymbolRef elf_sym(sym);

    StringRef bb_func_name;
    bool is_func = (type == llvm::object::SymbolRef::ST_Function);
    bool is_bb = SymbolEntry::IsBbSymbol(name, &bb_func_name);
    if (!is_func && !is_bb) continue;

    uint64_t size =  elf_sym.getSize();
    auto &addr_sym_list = address_map_[addr];
    if (!addr_sym_list.empty()) {
      // If we already have a symbol at the same address, try merging.
      SymbolEntry *aliased_symbol = nullptr;
      for (auto &sym : addr_sym_list) {
        // If both are functions, then alias both and set the size to the larger
        // one.
        if (!SymbolEntry::IsBbSymbol(sym->name) && is_func) {
          sym->size = (sym->size > size ? sym->size : size);
          sym->aliases.push_back(name);
          aliased_symbol = sym.get();
          break;
        }
      }
      if (aliased_symbol) continue;
    }
    addr_sym_list.emplace_back(new SymbolEntry(
        0, name, SymbolEntry::AliasesTy({name}), addr, size, nullptr, 0));
  }  // end of iterating object symbols.

  return MapFunctionAndBasicBlockSymbols() && FixupNameMap();
}

// Parse perf data file.
Optional<LBRAggregation> PropellerWholeProgramInfo::ParsePerfData() {
  if (options_.perf_name.empty()) return llvm::None;
  std::string match_mmap_name = options_.binary_name;
  if (!options_.profiled_binary_name.empty())
    // If user specified "--profiled_binary_name", we use it.
    match_mmap_name = options_.profiled_binary_name;
  else if (!options_.ignore_build_id)
    // Set match_mmap_name to "", so PerfDataReader::SelectPerfInfo auto
    // picks filename based on build-id, if build id is present; otherwise,
    // PerfDataReader::SelectPerfInfo uses options_.binary_name to match mmap
    // event file name.
    match_mmap_name = "";

  if (!PerfDataReader().SelectPerfInfo(options_.perf_name, match_mmap_name,
                                       &binary_perf_info_)) {
    LOG(ERROR) << "Parse perf file failed or no mmap found.";
    return llvm::None;
  }

  if (binary_perf_info_.binary_mmaps.empty()) {
    LOG(ERROR) << "No mmap found, cannot proceed";
    return llvm::None;
  }
  stats_.binary_mmap_num = binary_perf_info_.binary_mmaps.size();
  LBRAggregation lbr_aggregation;
  perf_data_reader_.AggregateLBR(binary_perf_info_, &lbr_aggregation);
  stats_.br_counters_accumulated = std::accumulate(
      lbr_aggregation.branch_counters.begin(),
      lbr_aggregation.branch_counters.end(), 0,
      [](uint64_t cnt, typename BranchCountersTy::value_type &v) {
        return cnt + v.second;
      });
  if (stats_.br_counters_accumulated <= 100)
    LOG(WARNING) << "Too few branch records in perf data.";
  return Optional<LBRAggregation>(std::move(lbr_aggregation));
}

// TODO(b/162192070): cleanup after migrating to bbinfo.
// Return a pair of (FunctionSymbol *, a vector of <SymbolEntries> that are to
// be turned into CFGNodes).
template <class C>
static std::pair<SymbolEntry *, std::vector<SymbolEntry *>>
GetFunctionAndBasicBlockSymbols(typename C::value_type &item) {
  CHECK(!item.second.empty());
  if constexpr (std::is_same_v<PropellerWholeProgramInfo::NameMapTy, C>) {
    // BasicBlock Labels workflow.
    std::vector<SymbolEntry *> result;
    result.reserve(item.second.size());
    for (auto &[symbol_name, symbol_entry] : item.second)
      result.push_back(symbol_entry);
    return {result[0], result};
  } else {
    return {item.second[0]->func_ptr, item.second};
  }
  // Unreachable.
  return {};
}

// A non-templated "DoCreateCfgs" is needed. Because there are unittests that
// directly calls DoCreateCfgs with LBRAggregation.
// TODO(b/162192070): cleanup after migrating to bbinfo.
void PropellerWholeProgramInfo::DoCreateCfgs(LBRAggregation &&lbr_aggregation) {
  if (binary_has_bb_addr_map_section())
    DoCreateCfgs(bb_addr_map_, std::move(lbr_aggregation));
  else
    DoCreateCfgs(name_map_, std::move(lbr_aggregation));
}

// Create control flow graph from symbol information and perf data. CFGNodes are
// created from "name_map_" or "bb_addr_map_" and template type "C" is used to
// represent either name_map_ or bb_addr_map_.
// TODO(b/162192070): remove template parameter after migrating to bbinfo.
template <class C>
void PropellerWholeProgramInfo::DoCreateCfgs(C &bb_groups,
                                             LBRAggregation &&lbr_aggregation) {
  std::map<const SymbolEntry *, std::vector<SymbolEntry *>, SymbolPtrComparator>
      largest_alias_syms;
  for (auto &bb_group : bb_groups) {
    auto [func_symbol, syms] = GetFunctionAndBasicBlockSymbols<C>(bb_group);

    auto [cur_iter, inserted] = largest_alias_syms.emplace(func_symbol, syms);
    if (!inserted && syms.size() > cur_iter->second.size()) {
      cur_iter->second = syms;
    }
  }

  // Temp map from SymbolEntry -> CFGNode.
  std::map<const SymbolEntry *, CFGNode *, SymbolPtrComparator> tmp_node_map;
  // Temp map from SymbolEntry -> ControlFlowGraph.
  std::map<const SymbolEntry *, ControlFlowGraph *, SymbolPtrComparator>
      tmp_cfg_map;
  for (auto& [func_symbol, syms] : largest_alias_syms) {
    CHECK(!syms.empty());
    CHECK(func_symbol);
    // TODO(b/162192070): remove template parameter after migrating to bbinfo.
    std::sort(syms.begin(), syms.end(), SymbolPtrComparator());

    auto i = tmp_cfg_map.find(func_symbol);
    // cfg with symbol func_symbol may already exist, because the same
    // SymbolEntry may occupy multiple slots of name_map_, because of aliases.
    if (i != tmp_cfg_map.end()) continue;

    ControlFlowGraph *cfg = new ControlFlowGraph();
    cfg->names_ = func_symbol->aliases;
    tmp_cfg_map.emplace(func_symbol, cfg);

    cfg->CreateNodes(syms);
    stats_.nodes_created += cfg->nodes_.size();
    // Setup mapping from syms <-> nodes.
    assert(cfg->nodes_.size() == syms.size());
    std::vector<SymbolEntry *>::iterator symsi = syms.begin();
    for (const auto &nptr : cfg->nodes_) {
      CHECK((*symsi)->ordinal == nptr.get()->symbol_ordinal_);
      tmp_node_map[*(symsi++)] = nptr.get();
    }
    ++stats_.cfgs_created;
  }
  CreateEdges(lbr_aggregation, tmp_node_map);

  for (auto &cfgi : tmp_cfg_map) {
    ControlFlowGraph *cfg_ptr = cfgi.second;
    // TODO(b/162192070): cleanup after removing bblabels workflow.
    // Only adjust CFGNode size for basic block labels workflow.
    cfg_ptr->FinishCreatingControlFlowGraph(
        std::is_same<PropellerWholeProgramInfo::NameMapTy, C>::value);
    cfgs_.emplace(std::piecewise_construct,
                  std::forward_as_tuple(cfgi.first->name),
                  std::forward_as_tuple(cfg_ptr));
  }

  // Release / cleanup.
  if (!options_.keep_frontend_intermediate_data) {
    name_map_.clear();
    binary_perf_info_.binary_mmaps.clear();
    lbr_aggregation = LBRAggregation();
    // Release ownership and delete SymbolEntry instances.
    address_map_.clear();
    symtab_.clear();
    bb_addr_map_.clear();
  }
}

CFGEdge *PropellerWholeProgramInfo::InternalCreateEdge(
    const SymbolEntry *from_sym, const SymbolEntry *to_sym, uint64_t weight,
    CFGEdge::Info edge_inf,
    const std::map<const SymbolEntry *, CFGNode *, SymbolPtrComparator>
        &tmp_node_map,
    std::map<SymbolPtrPair, CFGEdge *, SymbolPtrPairComparator> *tmp_edge_map) {
  CFGEdge *edge = nullptr;
  auto i = tmp_edge_map->find(std::make_pair(from_sym, to_sym));
  if (i != tmp_edge_map->end()) {
    edge = i->second;
    if (edge->info_ != edge_inf) {
      LOG(WARNING) << "Edges with same src and sink have different type: "
                   << CFGEdgeNameFormatter(edge) << " has type " << edge_inf
                   << " and " << edge->info_;
      stats_.edges_with_same_src_sink_but_different_type++;
    }
    edge->weight_ += weight;
  } else {
    auto from_ni = tmp_node_map.find(from_sym),
         to_ni = tmp_node_map.find(to_sym);
    if (from_ni == tmp_node_map.end() || to_ni == tmp_node_map.end())
      return nullptr;
    CFGNode *from_node = from_ni->second;
    CFGNode *to_node = to_ni->second;
    assert(from_node && to_node);
    edge = from_node->cfg_->CreateEdge(from_node, to_node, weight, edge_inf);
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
void PropellerWholeProgramInfo::CreateEdges(
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

  for (const typename BranchCountersTy::value_type &bcnt :
       lbr_aggregation.branch_counters) {
    uint64_t from = bcnt.first.first;
    uint64_t to = bcnt.first.second;
    uint64_t weight = bcnt.second;
    const SymbolEntry *from_sym = FindSymbolUsingBinaryAddress(from);
    const SymbolEntry *to_sym = FindSymbolUsingBinaryAddress(to);
    if (!from_sym || !to_sym) continue;

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

    CFGEdge::Info edge_inf = CFGEdge::DEFAULT;
    if (to_sym->func_ptr->addr == to)
      edge_inf = CFGEdge::CALL;
    else if (to != to_sym->addr || from_sym->IsReturnBlock())
      edge_inf = CFGEdge::RET;

    InternalCreateEdge(from_sym, to_sym, weight, edge_inf, tmp_node_map,
                       &tmp_edge_map);
  }

  CreateFallthroughs(lbr_aggregation, tmp_node_map,
                     &tmp_bb_fallthrough_counters, &tmp_edge_map);
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
      auto *fallthrough_edge =
          InternalCreateEdge(from_sym, to_sym, weight, CFGEdge::DEFAULT,
                             tmp_node_map, tmp_edge_map);
      if (!fallthrough_edge) break;
      fallthrough_edge->src_->fallthrough_edge_ = fallthrough_edge;
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

  // TODO(rahmanl): Uncomment this. Ref: b/154263650.
  // if (!from->isFallthroughBlock()) {
  //   LOG(WARNING) << "*** Skipping non-fallthrough ***" << from->name.str() ;
  //   return false;
  // }

  auto p = address_map_.find(from.addr), q = address_map_.find(to.addr),
       e = address_map_.end();
  if (p == e || q == e) {
    LOG(FATAL) << "*** Internal error: invalid symbol in fallthrough pair. ***";
    return false;
  }
  q = std::next(q);
  if (from.func_ptr != to.func_ptr) {
    LOG(ERROR) << "fallthrough (" << SymbolNameFormatter(from) << " -> "
               << SymbolNameFormatter(to)
               << ") does not start and end within the same faunction.";
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
    LOG(WARNING) << "More than 200 BBs along fallthrough ("
                 << SymbolNameFormatter(from) << " -> "
                 << SymbolNameFormatter(to) << "): " << std::dec << path->size()
                 << " BBs.";
  return true;
}

bool PropellerWholeProgramInfo::CreateCfgs() {
  // TODO(b/160191690): Replace usage of thread::Fiber and thread::Channel with
  // std::thread before open source.
  std::promise<bool> read_symbols_promise;
  std::future<bool> future_result = read_symbols_promise.get_future();
  std::thread populate_symbol_map_thread(
      &PropellerWholeProgramInfo::PopulateSymbolMapFromBbAddrMap, this,
      std::move(read_symbols_promise));

  Optional<LBRAggregation> opt_lbr_aggregation = ParsePerfData();
  populate_symbol_map_thread.join();
  if (!future_result.get() || !opt_lbr_aggregation) return false;

  // This must be done only after both PopulateSymbolMaps and ParsePerfData
  // finish. Note: opt_lbr_aggregation is released afterwards.
  DoCreateCfgs(std::move(*opt_lbr_aggregation));

  return true;
}
}  // namespace devtools_crosstool_autofdo
