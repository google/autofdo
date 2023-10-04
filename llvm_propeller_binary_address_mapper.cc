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
#include "binary_address_branch_path.h"
#include "lbr_aggregation.h"
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
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/log/log.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/time/time.h"
#include "third_party/abseil/absl/types/span.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "status_macros.h"

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
  Expected<std::vector<BBAddrMap>> bb_addr_map = elf_object->readBBAddrMap();
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
    auto iter = symtab.find(bb_addr_map[function_index].Addr);
    if (iter == symtab.end()) {
      LOG(WARNING) << "BB address map for function at "
                   << absl::StrCat(absl::Hex(bb_addr_map[function_index].Addr))
                   << " has no associated symbol table entry!";
      continue;
    }
    BinaryAddressMapper::FunctionSymbolInfo symbol_info;
    for (const llvm::object::ELFSymbolRef sr : iter->second)
      symbol_info.aliases.push_back(llvm::cantFail(sr.getName()));
    StringRef section_name = llvm::cantFail(
        llvm::cantFail(iter->second.front().getSection())->getName());
    symbol_info.section_name =
        (section_name == ".text" || section_name.startswith(".text."))
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
  // `lbr_aggregation != nullptr` only selects the hot functions in
  // `lbr_aggregation`. Otherwise, all functions are included. Does not take
  // ownership of `lbr_aggregation`, which must outlive this call.
  std::unique_ptr<BinaryAddressMapper> Build(
      const LbrAggregation *lbr_aggregation) &&;

 private:
  // Returns a list of hot functions based on profile `lbr_aggregation`. This
  // must be called after `ReadSymbolTable`, which initializes `symtab_`.
  // The returned `btree_set`
  // specifies the hot functions by their index in `bb_addr_map()`.
  absl::btree_set<int> CalculateHotFunctions(
      const LbrAggregation &lbr_aggregation);

  // Removes unwanted functions from the BB address map and symbol table, and
  // returns the remaining functions by their indexes in `bb_addr_map()`.
  // This function removes all non-text functions, functions without associated
  // names, and those with duplicate names. Selects all functions when
  // `lbr_aggregation == nullptr`.
  absl::btree_set<int> SelectFunctions(const LbrAggregation *lbr_aggregation);

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

// Helper class for extracting intra-function paths from binary-address paths.
// Example usage:
//   IntraFunctionPathsExtractor(&binary_address_mapper).Extract();
class IntraFunctionPathsExtractor {
 public:
  // Does not take ownership of `address_mapper` which should point to a valid
  // object which outlives the constructed `IntraFunctionPathsExtractor`.
  explicit IntraFunctionPathsExtractor(
      const BinaryAddressMapper *address_mapper)
      : address_mapper_(address_mapper) {}

  IntraFunctionPathsExtractor(const IntraFunctionPathsExtractor &) = delete;
  IntraFunctionPathsExtractor &operator=(const IntraFunctionPathsExtractor &) =
      delete;
  IntraFunctionPathsExtractor(IntraFunctionPathsExtractor &&) = default;
  IntraFunctionPathsExtractor &operator=(IntraFunctionPathsExtractor &&) =
      default;

  // Merges adjacent callsite branches by merging all of their calls into the
  // first one, while keeping the order.
  void MergeCallsites(std::vector<BbHandleBranchPath> &paths) {
    for (auto &path : paths) {
      BbHandleBranch *prev_branch = &*path.branches.begin();
      path.branches.erase(
          std::remove_if(
              path.branches.begin() + 1, path.branches.end(),
              [&](BbHandleBranch &branch) {
                if (prev_branch->is_callsite() && branch.is_callsite() &&
                    prev_branch->from_bb == branch.from_bb) {
                  CHECK(prev_branch->from_bb == prev_branch->to_bb)
                      << prev_branch
                      << " is not a callsite in a single "
                         "block.";
                  absl::c_move(branch.call_rets,
                               std::back_inserter(prev_branch->call_rets));
                  return true;
                }
                prev_branch = &branch;
                return false;
              }),
          path.branches.end());
    }
  }

  // Extracts and returns the intra-function paths in `address_path`.
  std::vector<BbHandleBranchPath> Extract(
      const BinaryAddressBranchPath &address_path) && {
    pid_ = address_path.pid;
    sample_time_ = address_path.sample_time;

    // Helper function to get the BB handle associated with an index, or
    // nullopt if the index is nullopt.
    auto GetBbHandleByIndex =
        [&](std::optional<int> index) -> std::optional<BbHandle> {
      if (!index.has_value()) return std::nullopt;
      return address_mapper_->bb_handles().at(*index);
    };

    for (const BinaryAddressBranch &branch : address_path.branches) {
      std::optional<BbHandle> from_bb_handle = GetBbHandleByIndex(
          address_mapper_->FindBbHandleIndexUsingBinaryAddress(
              branch.from, BranchDirection::kFrom));
      std::optional<BbHandle> to_bb_handle = GetBbHandleByIndex(
          address_mapper_->FindBbHandleIndexUsingBinaryAddress(
              branch.to, BranchDirection::kTo));

      if (from_bb_handle.has_value()) {
        // Augment the current path if the current path is from the same
        // function and ends at a known address. Otherwise switch to a new path.
        if (from_bb_handle->function_index == current_function_index_ &&
            GetCurrentLastBranch().to_bb.has_value()) {
          AugmentCurrentPath({.from_bb = from_bb_handle});
        } else {
          AddNewPath({.from_bb = from_bb_handle});
        }
      }
      if (!to_bb_handle.has_value()) continue;
      if (address_mapper_->IsCall(*to_bb_handle, branch.to)) {
        HandleCall(from_bb_handle, *to_bb_handle);
        continue;
      }
      if (address_mapper_->IsReturn(from_bb_handle, *to_bb_handle, branch.to)) {
        HandleReturn(from_bb_handle, *to_bb_handle);
        continue;
      }
      // Not a call or a return. It must be a normal branch within the same
      // function.
      CHECK(from_bb_handle.has_value());
      HandleRegularBranch(*from_bb_handle, *to_bb_handle);
    }
    MergeCallsites(paths_);
    return std::move(paths_);
  }

 private:
  // Extends the current path by adding a regular branch `from_bb_handle` to
  // `to_bb_handle`, which is intra-function and not call or return. Assumes and
  // verifies that `GetCurrentLastBranch()` already has its source assigned as
  // `from_bb_handle` and then assigns its sink to `to_bb_handle`.
  void HandleRegularBranch(BbHandle from_bb_handle, BbHandle to_bb_handle) {
    CHECK_EQ(from_bb_handle.function_index, to_bb_handle.function_index);
    auto &last_branch = GetCurrentLastBranch();
    CHECK(last_branch.from_bb.has_value());
    CHECK(*last_branch.from_bb == from_bb_handle);
    last_branch.to_bb = to_bb_handle;
  }

  // Handles a call from `from_bb_handle` to `to_bb_handle`. Stores the current
  // path in the stack and inserts and switches to a new path starting with
  // `to_bb_handle`.
  void HandleCall(std::optional<BbHandle> from_bb_handle,
                  BbHandle to_bb_handle) {
    if (from_bb_handle.has_value()) {
      // Pop the current path off the call stack if the from bb has a tail call.
      // Note that this may incorrectly pop off the call stack for a regular
      // call located in a block ending with a tail call. However, popping off
      // the stack will make the paths shorter, but won't affect correctness.
      if (address_mapper_->GetBBEntry(*from_bb_handle).hasTailCall())
        call_stack_[current_function_index_].pop();
      GetCurrentLastBranch().call_rets.push_back(
          {.callee = to_bb_handle.function_index});
    }
    AddNewPath({.to_bb = to_bb_handle});
  }

  // Handles a return from `from_bb_handle` to `to_bb_handle`. Terminates the
  // path corresponding to the callee. Then tries to find and switch to the path
  // corresponding to the callsite of this return. Starts a new path if the
  // caller path was not found.
  void HandleReturn(std::optional<BbHandle> from_bb_handle,
                    BbHandle to_bb_handle) {
    // Set the returns_to block and pop off the call stack if the return is from
    // a known BB.
    if (from_bb_handle.has_value()) {
      paths_[current_path_index_].returns_to = to_bb_handle;
      call_stack_[current_function_index_].pop();
    }
    // Find the path corresponding to the callsite.
    auto it = call_stack_.find(to_bb_handle.function_index);
    if (it == call_stack_.end() || it->second.empty()) {
      // The callsite path doesn't exist in this trace.
      AddNewPath({.to_bb = to_bb_handle,
                  .call_rets = {CallRetInfo{.return_bb = from_bb_handle}}});
      return;
    }
    current_path_index_ = it->second.top();
    BbHandleBranch &callsite_branch = GetCurrentLastBranch();

    if (callsite_branch.to_bb.has_value()) {
      LOG(INFO) << "Found corrupt callsite path while assigning sink: "
                << to_bb_handle << " branched-to from: " << from_bb_handle
                << " (path's last branch already has a sink): "
                << paths_[current_path_index_];
      AddNewPath({.to_bb = to_bb_handle});
      return;
    }
    CHECK(callsite_branch.from_bb.has_value());
    BbHandle &callsite_bb = *callsite_branch.from_bb;
    CHECK_EQ(callsite_bb.function_index, to_bb_handle.function_index);
    // Check that the returned-to block is the same as the callsite block or
    // immediately after.  Start a new path if found otherwise.
    if (to_bb_handle.bb_index != callsite_bb.bb_index &&
        address_mapper_->GetAddress(to_bb_handle) !=
            address_mapper_->GetEndAddress(callsite_bb)) {
      LOG(INFO)
          << "Found corrupt callsite path while assigning sink: "
          << to_bb_handle << " branched-to from: " << from_bb_handle
          << " (return address does not fall immediately after the call): "
          << paths_[current_path_index_];
      AddNewPath({.to_bb = to_bb_handle});
      return;
    }
    // Insert a new `CallRetInfo` or assign `return_bb` of the last one.
    if (callsite_branch.call_rets.empty()) {
      callsite_branch.call_rets.push_back(
          CallRetInfo{.return_bb = from_bb_handle});
    } else {
      if (callsite_branch.call_rets.back().return_bb.has_value()) {
        callsite_branch.call_rets.push_back(
            CallRetInfo{.return_bb = from_bb_handle});
      } else {
        callsite_branch.call_rets.back().return_bb = from_bb_handle;
      }
    }
    // Assign the sink of the last branch. This can be a return back to the
    // same block or the next (when the call instruction is the last
    // instruction of the block).
    callsite_branch.to_bb = to_bb_handle;
    current_function_index_ = to_bb_handle.function_index;
  }

  // Inserts `bb_branch` at the end of the current path.
  void AugmentCurrentPath(const BbHandleBranch &bb_branch) {
    paths_[current_path_index_].branches.push_back(bb_branch);
  }

  // Adds a new path with a single branch `bb_branch` and updates
  // `current_path_index_` and `call_stack_`.
  void AddNewPath(const BbHandleBranch &bb_branch) {
    current_function_index_ = bb_branch.from_bb.has_value()
                                  ? bb_branch.from_bb->function_index
                                  : bb_branch.to_bb->function_index;
    paths_.push_back({.pid = pid_, .branches = {bb_branch}});
    current_path_index_ = paths_.size() - 1;
    call_stack_[current_function_index_].push(current_path_index_);
  }

  BbHandleBranch &GetCurrentLastBranch() {
    CHECK_GE(current_path_index_, 0);
    CHECK(!paths_[current_path_index_].branches.empty());
    return paths_[current_path_index_].branches.back();
  }

  const BinaryAddressMapper *address_mapper_ = nullptr;
  // Process id associated with the path.
  int64_t pid_ = -1;
  // Sample time associated with the path.
  absl::Time sample_time_ = absl::InfinitePast();
  // Index of the current function in address_mapper_->bb_addr_map().
  int current_function_index_ = -1;
  std::vector<BbHandleBranchPath> paths_;
  // Index of the current path in `paths_`.
  int current_path_index_ = -1;
  // Call stack map indexed by function index, mapping to path indices in
  // `paths_` in the calling stack order.
  absl::flat_hash_map<int, std::stack<int>> call_stack_;
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
      LOG(FATAL) << "Invalid edge direction.";
  }
}

bool BinaryAddressMapper::CanFallThrough(int from, int to) const {
  if (from == to) return true;
  BbHandle from_bb = bb_handles_[from];
  BbHandle to_bb = bb_handles_[to];
  if (from_bb.function_index != to_bb.function_index) {
    LOG_EVERY_N(ERROR, 100)
        << "Skipping fallthrough path " << from_bb << "->" << to_bb
        << ": endpoints are in different functions.";
    return false;
  }
  if (from_bb.bb_index > to_bb.bb_index) {
    LOG_EVERY_N(WARNING, 100) << "Skipping fallthrough path " << from_bb << "->"
                              << to_bb << ": start comes after end.";
    return false;
  }
  for (int i = from_bb.bb_index; i != to_bb.bb_index; ++i) {
    BbHandle bb_sym = {.function_index = from_bb.function_index, .bb_index = i};
    // (b/62827958) Sometimes LBR contains duplicate entries in the beginning
    // of the stack which may result in false fallthrough paths. We discard
    // the fallthrough path if any intermediate block (except the destination
    // block) does not fall through (source block is checked before entering
    // this loop).
    if (!GetBBEntry(bb_sym).canFallThrough()) {
      LOG_EVERY_N(WARNING, 100)
          << "Skipping fallthrough path " << from_bb << "->" << to_bb
          << ": covers non-fallthrough block " << bb_sym << ".";
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
    const LbrAggregation &lbr_aggregation) {
  absl::btree_set<int> hot_functions;
  auto add_to_hot_functions = [this, &hot_functions](uint64_t binary_address) {
    auto it =
        absl::c_upper_bound(bb_addr_map_, binary_address,
                            [](uint64_t addr, const BBAddrMap &func_entry) {
                              return addr < func_entry.Addr;
                            });
    if (it == bb_addr_map_.begin()) return;
    it = std::prev(it);
    // We know the address is bigger than or equal to the function address. Make
    // sure that it doesn't point beyond the last basic block.
    if (binary_address >=
        it->Addr + it->BBEntries.back().Offset + it->BBEntries.back().Size)
      return;
    hot_functions.insert(it - bb_addr_map_.begin());
  };
  for (const auto &bcnt : lbr_aggregation.branch_counters) {
    add_to_hot_functions(bcnt.first.from);
    add_to_hot_functions(bcnt.first.to);
  }
  stats_->hot_functions = hot_functions.size();
  return hot_functions;
}

void BinaryAddressMapperBuilder::DropNonSelectedFunctions(
    const absl::btree_set<int> &selected_functions) {
  for (int i = 0; i != bb_addr_map_.size(); ++i) {
    if (selected_functions.contains(i)) continue;
    bb_addr_map_[i].BBEntries.clear();
    bb_addr_map_[i].BBEntries.shrink_to_fit();
    symbol_info_map_.erase(i);
  }
}

void BinaryAddressMapperBuilder::FilterNoNameFunctions(
    absl::btree_set<int> &selected_functions) const {
  for (auto it = selected_functions.begin(); it != selected_functions.end();) {
    if (!symbol_info_map_.contains(*it)) {
      LOG(WARNING) << "Hot function at address: 0x"
                   << absl::StrCat(absl::Hex(bb_addr_map_[*it].Addr))
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
        return absl::c_equal(func_addr_map.BBEntries, bb_addr_map_[i].BBEntries,
                             [](const llvm::object::BBAddrMap::BBEntry &e1,
                                const llvm::object::BBAddrMap::BBEntry &e2) {
                               return e1.Offset == e2.Offset &&
                                      e1.Size == e2.Size;
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
    const LbrAggregation *lbr_aggregation) {
  absl::btree_set<int> selected_functions;
  if (lbr_aggregation != nullptr) {
    selected_functions = CalculateHotFunctions(*lbr_aggregation);
  } else {
    for (int i = 0; i != bb_addr_map_.size(); ++i) selected_functions.insert(i);
  }

  FilterNoNameFunctions(selected_functions);
  if (options_->filter_non_text_functions())
    FilterNonTextFunctions(selected_functions);
  stats_->duplicate_symbols += FilterDuplicateNameFunctions(selected_functions);
  return selected_functions;
}

std::vector<BbHandleBranchPath> BinaryAddressMapper::ExtractIntraFunctionPaths(
    const BinaryAddressBranchPath &address_path) const {
  return IntraFunctionPathsExtractor(this).Extract(address_path);
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
  stats_->bbaddrmap_function_does_not_have_symtab_entry +=
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
    PropellerStats &stats, const LbrAggregation *lbr_aggregation) {
  LOG(INFO) << "Started reading the binary content from: "
            << binary_content.file_name;
  absl::flat_hash_map<uint64_t, llvm::SmallVector<llvm::object::ELFSymbolRef>>
      symtab = ReadSymbolTable(binary_content);
  std::vector<llvm::object::BBAddrMap> bb_addr_map;
  ASSIGN_OR_RETURN(bb_addr_map, ReadBbAddrMap(binary_content));
  return BinaryAddressMapperBuilder(std::move(symtab), std::move(bb_addr_map),
                                    stats, &options)
      .Build(lbr_aggregation);
}

std::unique_ptr<BinaryAddressMapper> BinaryAddressMapperBuilder::Build(
    const LbrAggregation *lbr_aggregation) && {
  std::optional<uint64_t> last_function_address;
  std::vector<BbHandle> bb_handles;
  absl::btree_set<int> selected_functions = SelectFunctions(lbr_aggregation);
  DropNonSelectedFunctions(selected_functions);
  for (int function_index : selected_functions) {
    const auto &function_bb_addr_map = bb_addr_map_[function_index];
    if (last_function_address.has_value())
      CHECK_GT(function_bb_addr_map.Addr, *last_function_address);
    for (int bb_index = 0; bb_index != function_bb_addr_map.BBEntries.size();
         ++bb_index)
      bb_handles.push_back({function_index, bb_index});
    last_function_address = function_bb_addr_map.Addr;
  }
  return std::make_unique<BinaryAddressMapper>(
      std::move(selected_functions), std::move(bb_addr_map_),
      std::move(bb_handles), std::move(symbol_info_map_));
}

}  // namespace devtools_crosstool_autofdo
