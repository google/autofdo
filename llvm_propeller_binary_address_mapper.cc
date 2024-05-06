#include "llvm_propeller_binary_address_mapper.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <stack>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "bb_handle.h"
#include "binary_address_branch.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/base/attributes.h"
#include "third_party/abseil/absl/base/nullability.h"
#include "third_party/abseil/absl/container/btree_set.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/time/time.h"
#include "third_party/abseil/absl/types/span.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "base/logging.h"
#include "base/status_macros.h"

namespace devtools_crosstool_autofdo {

namespace {
using ::llvm::Expected;
using ::llvm::StringRef;
using ::llvm::object::BBAddrMap;

// Returns the binary's function symbols by reading from its symbol table.
absl::flat_hash_map<uint64_t, llvm::SmallVector<llvm::object::ELFSymbolRef>>
ReadSymbolTable(const BinaryContent &binary_content) {
  absl::flat_hash_map<uint64_t, llvm::SmallVector<llvm::object::ELFSymbolRef>>
      symtab;
  for (llvm::object::SymbolRef sr : binary_content.object_file->symbols()) {
    llvm::object::ELFSymbolRef symbol(sr);
    uint8_t stt = symbol.getELFType();
    if (stt != llvm::ELF::STT_FUNC) continue;
    Expected<uint64_t> address = sr.getAddress();
    if (!address || !*address) continue;
    Expected<StringRef> func_name = symbol.getName();
    if (!func_name) continue;
    const uint64_t func_size = symbol.getSize();
    if (func_size == 0) continue;

    auto &addr_sym_list = symtab[*address];
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
  return symtab;
}

// Returns the binary's `BBAddrMap`s by calling LLVM-side decoding function
// `ELFObjectFileBase::readBBAddrMap`. Returns error if the call fails or if the
// result is empty.
absl::StatusOr<std::vector<BBAddrMap>> ReadBbAddrMap(
    const BinaryContent &binary_content) {
  auto *elf_object = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(
      binary_content.object_file.get());
  CHECK_NE(elf_object, nullptr);
  Expected<std::vector<BBAddrMap>> bb_addr_map = elf_object->readBBAddrMap(
      binary_content.kernel_module.has_value()
          ? std::optional<unsigned>(
                binary_content.kernel_module->text_section_index)
          : std::nullopt);
  if (!bb_addr_map) {
    return absl::InternalError(
        llvm::formatv(
            "Failed to read the LLVM_BB_ADDR_MAP section from {0}: {1}.",
            binary_content.file_name,
            llvm::fmt_consume(bb_addr_map.takeError()))
            .str());
  }
  if (bb_addr_map->empty()) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "'%s' does not have a non-empty LLVM_BB_ADDR_MAP section.",
        binary_content.file_name));
  }
  return std::move(*bb_addr_map);
}

// Returns a map from BB-address-map function indexes to their symbol info.
absl::flat_hash_map<int, BinaryAddressMapper::FunctionSymbolInfo>
GetSymbolInfoMap(
    const absl::flat_hash_map<
        uint64_t, llvm::SmallVector<llvm::object::ELFSymbolRef>> &symtab,
    absl::Span<const BBAddrMap> bb_addr_map) {
  absl::flat_hash_map<int, BinaryAddressMapper::FunctionSymbolInfo>
      symbol_info_map;
  absl::flat_hash_set<StringRef> section_names;
  for (int function_index = 0; function_index != bb_addr_map.size();
       ++function_index) {
    auto iter = symtab.find(bb_addr_map[function_index].getFunctionAddress());
    if (iter == symtab.end()) {
      LOG(WARNING) << "BB address map for function at "
                   << absl::StrCat(absl::Hex(
                          bb_addr_map[function_index].getFunctionAddress()))
                   << " has no associated symbol table entry!";
      continue;
    }
    BinaryAddressMapper::FunctionSymbolInfo symbol_info;
    for (const llvm::object::ELFSymbolRef sr : iter->second)
      symbol_info.aliases.push_back(llvm::cantFail(sr.getName()));
    StringRef section_name = llvm::cantFail(
        llvm::cantFail(iter->second.front().getSection())->getName());
    symbol_info.section_name =
        (section_name == ".text" || section_name.starts_with(".text."))
            ? section_name.substr(0, 5)
            : section_name;
    symbol_info_map.emplace(function_index, std::move(symbol_info));
  }
  return symbol_info_map;
}

// Builds `BinaryAddressMapper` for a binary and its profile.
class BinaryAddressMapperBuilder {
 public:
  BinaryAddressMapperBuilder(
      absl::flat_hash_map<uint64_t,
                          llvm::SmallVector<llvm::object::ELFSymbolRef>>
          symtab,
      std::vector<llvm::object::BBAddrMap> bb_addr_map, PropellerStats &stats,
      absl::Nonnull<const PropellerOptions *> options
          ABSL_ATTRIBUTE_LIFETIME_BOUND);

  BinaryAddressMapperBuilder(const BinaryAddressMapperBuilder &) = delete;
  BinaryAddressMapperBuilder &operator=(const BinaryAddressMapper &) = delete;
  BinaryAddressMapperBuilder(BinaryAddressMapperBuilder &&) = delete;
  BinaryAddressMapperBuilder &operator=(BinaryAddressMapperBuilder &&) = delete;

  // Builds and returns a `BinaryAddressMapper`. When
  // `hot_addresses != nullptr` only selects functions with addresses in
  // `*hot_addresses`. Otherwise, all functions are included. Does not take
  // ownership of `*hot_addresses`, which must outlive this call.
  std::unique_ptr<BinaryAddressMapper> Build(
      const absl::flat_hash_set<uint64_t> *hot_addresses) &&;

 private:
  // Returns a list of hot functions based on addresses `hot_addresses`. This
  // must be called after `ReadSymbolTable`, which initializes `symtab_`.
  // The returned `btree_set`
  // specifies the hot functions by their index in `bb_addr_map()`.
  absl::btree_set<int> CalculateHotFunctions(
      const absl::flat_hash_set<uint64_t> &hot_addresses);

  // Removes unwanted functions from the BB address map and symbol table, and
  // returns the remaining functions by their indexes in `bb_addr_map()`.
  // This function removes all non-text functions, functions without associated
  // names, and those with duplicate names. Selects all functions when
  // `hot_addresses == nullptr`.
  absl::btree_set<int> SelectFunctions(
      const absl::flat_hash_set<uint64_t> *hot_addresses);

  // Removes all functions that are not included (selected) in the
  // `selected_functions` set. Clears their associated BB entries from
  // `bb_addr_map_` and also removes their associated entries from `symtab_`.
  void DropNonSelectedFunctions(const absl::btree_set<int> &selected_functions);

  // Removes all functions without associated symbol names from the given
  // function indices.
  void FilterNoNameFunctions(absl::btree_set<int> &selected_functions) const;

  // Removes all functions in non-text sections from the specified set of
  // function indices.
  void FilterNonTextFunctions(absl::btree_set<int> &selected_functions) const;

  // Removes all functions with duplicate names from the specified function
  // indices. Must be called after `FilterNoNameFunctions`.
  int FilterDuplicateNameFunctions(
      absl::btree_set<int> &selected_functions) const;

  // BB address map of functions.
  std::vector<llvm::object::BBAddrMap> bb_addr_map_;
  // Non-zero sized function symbols from elf symbol table, indexed by
  // symbol address. Multiple function symbols may exist on the same address.
  absl::flat_hash_map<uint64_t, llvm::SmallVector<llvm::object::ELFSymbolRef>>
      symtab_;

  // Map from every function index (in `bb_addr_map_`) to its symbol info.
  absl::flat_hash_map<int, BinaryAddressMapper::FunctionSymbolInfo>
      symbol_info_map_;

  PropellerStats *stats_;
  const PropellerOptions *options_;
};
}  // namespace

std::optional<int> BinaryAddressMapper::FindBbHandleIndexUsingBinaryAddress(
    uint64_t address, BranchDirection direction) const {
  std::vector<BbHandle>::const_iterator it = absl::c_upper_bound(
      bb_handles_, address, [this](uint64_t addr, const BbHandle &bb_handle) {
        return addr < GetAddress(bb_handle);
      });
  if (it == bb_handles_.begin()) return std::nullopt;
  it = std::prev(it);
  if (address > GetAddress(*it)) {
    if (address >= GetAddress(*it) + GetBBEntry(*it).Size)
      return std::nullopt;
    else
      return it - bb_handles_.begin();
  }
  DCHECK_EQ(address, GetAddress(*it));
  // We might have multiple zero-sized BBs at the same address. If we are
  // branching to this address, we find and return the first zero-sized BB (from
  // the same function). If we are branching from this address, we return the
  // single non-zero sized BB.
  switch (direction) {
    case BranchDirection::kTo: {
      auto prev_it = it;
      while (prev_it != bb_handles_.begin() &&
             GetAddress(*--prev_it) == address &&
             prev_it->function_index == it->function_index) {
        it = prev_it;
      }
      return it - bb_handles_.begin();
    }
    case BranchDirection::kFrom: {
      DCHECK_NE(GetBBEntry(*it).Size, 0);
      return it - bb_handles_.begin();
    }
    default:
      LOG(FATAL) << "Invalid edge direction.";
  }
}

bool BinaryAddressMapper::CanFallThrough(int from, int to) const {
  if (from == to) return true;
  BbHandle from_bb = bb_handles_[from];
  BbHandle to_bb = bb_handles_[to];
  if (from_bb.function_index != to_bb.function_index) {
    LOG_EVERY_N(ERROR, 100) << absl::StrFormat(
        "Skipping fallthrough path %v->%v: endpoints are in different functions.",
        from_bb, to_bb);
    return false;
  }
  if (from_bb.bb_index > to_bb.bb_index) {
    LOG_EVERY_N(WARNING, 100) << absl::StrFormat(
        "Skipping fallthrough path %v->%v: start comes after end.",
        from_bb, to_bb);
    return false;
  }
  for (int i = from_bb.bb_index; i != to_bb.bb_index; ++i) {
    BbHandle bb_sym = {.function_index = from_bb.function_index, .bb_index = i};
    // Sometimes LBR contains duplicate entries in the beginning
    // of the stack which may result in false fallthrough paths. We discard
    // the fallthrough path if any intermediate block (except the destination
    // block) does not fall through (source block is checked before entering
    // this loop).
    if (!GetBBEntry(bb_sym).canFallThrough()) {
      LOG_EVERY_N(WARNING, 100) << absl::StrFormat(
          "Skipping fallthrough path %v->%v: covers non-fallthrough block %v.",
          from_bb, to_bb, bb_sym);
      return false;
    }
  }
  // Warn about unusually-long fallthroughs.
  if (to - from >= 200) {
    LOG(WARNING) << "More than 200 BBs along fallthrough (" << GetName(from_bb)
                 << " -> " << GetName(to_bb) << "): " << to - from + 1
                 << " BBs.";
  }
  return true;
}

// For each lbr record addr1->addr2, find function1/2 that contain addr1/addr2
// and add function1/2's index into the returned set.
absl::btree_set<int> BinaryAddressMapperBuilder::CalculateHotFunctions(
    const absl::flat_hash_set<uint64_t> &hot_addresses) {
  absl::btree_set<int> hot_functions;
  auto add_to_hot_functions = [this, &hot_functions](uint64_t binary_address) {
    auto it =
        absl::c_upper_bound(bb_addr_map_, binary_address,
                            [](uint64_t addr, const BBAddrMap &func_entry) {
                              return addr < func_entry.getFunctionAddress();
                            });
    if (it == bb_addr_map_.begin()) return;
    it = std::prev(it);
    // We know the address is bigger than or equal to the function address. Make
    // sure that it doesn't point beyond the last basic block.
    if (binary_address >= it->getFunctionAddress() +
                              it->getBBEntries().back().Offset +
                              it->getBBEntries().back().Size)
      return;
    hot_functions.insert(it - bb_addr_map_.begin());
  };
  for (uint64_t address : hot_addresses) add_to_hot_functions(address);
  stats_->bbaddrmap_stats.hot_functions = hot_functions.size();
  return hot_functions;
}

void BinaryAddressMapperBuilder::DropNonSelectedFunctions(
    const absl::btree_set<int> &selected_functions) {
  for (int i = 0; i != bb_addr_map_.size(); ++i) {
    if (selected_functions.contains(i)) continue;
    symbol_info_map_.erase(i);
  }
}

void BinaryAddressMapperBuilder::FilterNoNameFunctions(
    absl::btree_set<int> &selected_functions) const {
  for (auto it = selected_functions.begin(); it != selected_functions.end();) {
    if (!symbol_info_map_.contains(*it)) {
      LOG(WARNING) << "Hot function at address: 0x"
                   << absl::StrCat(
                          absl::Hex(bb_addr_map_[*it].getFunctionAddress()))
                   << " does not have an associated symbol name.";
      it = selected_functions.erase(it);
    } else {
      ++it;
    }
  }
}

void BinaryAddressMapperBuilder::FilterNonTextFunctions(
    absl::btree_set<int> &selected_functions) const {
  for (auto func_it = selected_functions.begin();
       func_it != selected_functions.end();) {
    int function_index = *func_it;
    const auto &symbol_info = symbol_info_map_.at(function_index);
    if (symbol_info.section_name != ".text") {
      LOG_EVERY_N(WARNING, 1000) << "Skipped symbol in non-'.text.*' section '"
                                 << symbol_info.section_name.str()
                                 << "': " << symbol_info.aliases.front().str();
      func_it = selected_functions.erase(func_it);
    } else {
      ++func_it;
    }
  }
}

// Without '-funique-internal-linkage-names', if multiple functions have the
// same name, even though we can correctly map their profiles, we cannot apply
// those profiles back to their object files.
// This function removes all such functions which have the same name as other
// functions in the binary.
int BinaryAddressMapperBuilder::FilterDuplicateNameFunctions(
    absl::btree_set<int> &selected_functions) const {
  int duplicate_symbols = 0;
  absl::flat_hash_map<StringRef, std::vector<int>> name_to_function_index;
  for (int func_index : selected_functions) {
    for (StringRef name : symbol_info_map_.at(func_index).aliases)
      name_to_function_index[name].push_back(func_index);
  }

  for (auto [name, func_indices] : name_to_function_index) {
    if (func_indices.size() <= 1) continue;
    duplicate_symbols += func_indices.size() - 1;
    // Sometimes, duplicated uniq-named symbols are essentially identical
    // copies. In such cases, we can still keep one copy.
    // TODO(rahmanl): Why does this work? If we remove other copies, we cannot
    // map their profiles either.
    if (name.contains(".__uniq.")) {
      // duplicate uniq-named symbols found
      const BBAddrMap &func_addr_map = bb_addr_map_[func_indices.front()];
      // If the uniq-named functions have the same structure, we assume
      // they are the same and thus we keep one copy of them.
      bool same_structure = absl::c_all_of(func_indices, [&](int i) {
        return absl::c_equal(
            func_addr_map.getBBEntries(), bb_addr_map_[i].getBBEntries(),
            [](const llvm::object::BBAddrMap::BBEntry &e1,
               const llvm::object::BBAddrMap::BBEntry &e2) {
              return e1.Offset == e2.Offset && e1.Size == e2.Size;
            });
      });
      if (same_structure) {
        LOG(WARNING) << func_indices.size()
                     << " duplicate uniq-named functions '" << name.str()
                     << "' with same size and structure found, keep one copy.";
        for (int i = 1; i < func_indices.size(); ++i)
          selected_functions.erase(func_indices[i]);
        continue;
      }
      LOG(WARNING) << "duplicate uniq-named functions '" << name.str()
                   << "' with different size or structure found , drop "
                      "all of them.";
    }
    for (auto func_idx : func_indices) selected_functions.erase(func_idx);
  }
  return duplicate_symbols;
}

absl::btree_set<int> BinaryAddressMapperBuilder::SelectFunctions(
    const absl::flat_hash_set<uint64_t> *hot_addresses) {
  absl::btree_set<int> selected_functions;
  if (hot_addresses != nullptr) {
    selected_functions = CalculateHotFunctions(*hot_addresses);
  } else {
    for (int i = 0; i != bb_addr_map_.size(); ++i) selected_functions.insert(i);
  }

  FilterNoNameFunctions(selected_functions);
  if (options_->filter_non_text_functions())
    FilterNonTextFunctions(selected_functions);
  stats_->bbaddrmap_stats.duplicate_symbols +=
      FilterDuplicateNameFunctions(selected_functions);
  return selected_functions;
}

BinaryAddressMapperBuilder::BinaryAddressMapperBuilder(
    absl::flat_hash_map<uint64_t, llvm::SmallVector<llvm::object::ELFSymbolRef>>
        symtab,
    std::vector<llvm::object::BBAddrMap> bb_addr_map, PropellerStats &stats,
    const PropellerOptions *options)
    : bb_addr_map_(std::move(bb_addr_map)),
      symtab_(std::move(symtab)),
      symbol_info_map_(GetSymbolInfoMap(symtab_, bb_addr_map_)),
      stats_(&stats),
      options_(options) {
  stats_->bbaddrmap_stats.bbaddrmap_function_does_not_have_symtab_entry +=
      bb_addr_map_.size() - symbol_info_map_.size();
}

BinaryAddressMapper::BinaryAddressMapper(
    absl::btree_set<int> selected_functions,
    std::vector<llvm::object::BBAddrMap> bb_addr_map,
    std::vector<BbHandle> bb_handles,
    absl::flat_hash_map<int, FunctionSymbolInfo> symbol_info_map)
    : selected_functions_(std::move(selected_functions)),
      bb_handles_(std::move(bb_handles)),
      bb_addr_map_(std::move(bb_addr_map)),
      symbol_info_map_(std::move(symbol_info_map)) {}

absl::StatusOr<std::unique_ptr<BinaryAddressMapper>> BuildBinaryAddressMapper(
    const PropellerOptions &options, const BinaryContent &binary_content,
    PropellerStats &stats, const absl::flat_hash_set<uint64_t> *hot_addresses) {
  LOG(INFO) << "Started reading the binary content from: "
            << binary_content.file_name;
  absl::flat_hash_map<uint64_t, llvm::SmallVector<llvm::object::ELFSymbolRef>>
      symtab = ReadSymbolTable(binary_content);
  std::vector<llvm::object::BBAddrMap> bb_addr_map;
  ASSIGN_OR_RETURN(bb_addr_map, ReadBbAddrMap(binary_content));

  return BinaryAddressMapperBuilder(std::move(symtab), std::move(bb_addr_map),
                                    stats, &options)
      .Build(hot_addresses);
}

std::unique_ptr<BinaryAddressMapper> BinaryAddressMapperBuilder::Build(
    const absl::flat_hash_set<uint64_t> *hot_addresses) && {
  std::optional<uint64_t> last_function_address;
  std::vector<BbHandle> bb_handles;
  absl::btree_set<int> selected_functions = SelectFunctions(hot_addresses);
  DropNonSelectedFunctions(selected_functions);
  for (int function_index : selected_functions) {
    const auto &function_bb_addr_map = bb_addr_map_[function_index];
    if (last_function_address.has_value())
      CHECK_GT(function_bb_addr_map.getFunctionAddress(),
               *last_function_address);
    for (int bb_index = 0;
         bb_index != function_bb_addr_map.getBBEntries().size(); ++bb_index)
      bb_handles.push_back({function_index, bb_index});
    last_function_address = function_bb_addr_map.getFunctionAddress();
  }
  return std::make_unique<BinaryAddressMapper>(
      std::move(selected_functions), std::move(bb_addr_map_),
      std::move(bb_handles), std::move(symbol_info_map_));
}

}  // namespace devtools_crosstool_autofdo
