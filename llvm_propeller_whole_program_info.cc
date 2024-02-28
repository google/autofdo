#include "llvm_propeller_whole_program_info.h"

#include <fcntl.h>  // for "O_RDONLY"

#include <algorithm>
#include <cstdint>
#include <future>  // NOLINT(build/c++11)
#include <ios>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "perfdata_reader.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/btree_set.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatAdapters.h"

// The following inclusions are automatically deleted when prepare source for
// upstream.

// github tree does not have util/task/status* files
// "ASSIGN_OR_RETURN" is similarly defined.
#if not defined(ASSIGN_OR_RETURN)
#define ASSIGN_OR_RETURN(lhs, exp) \
    auto _status_or_value ## __LINE__ = (exp); \
    if (!_status_or_value ## __LINE__.ok()) \
      return _status_or_value ## __LINE__.status(); \
    lhs = std::move(_status_or_value ## __LINE__.value());
#endif

namespace devtools_crosstool_autofdo {

namespace {
using ::llvm::Expected;
using ::llvm::StringRef;
using SymTabTy =
    ::devtools_crosstool_autofdo::PropellerWholeProgramInfo::SymTabTy;
using ::devtools_crosstool_autofdo::AddressFormatter;
using ::devtools_crosstool_autofdo::BinaryInfo;
using ::llvm::object::BBAddrMap;

// Returns the binary's function symbols by reading from its symbol table.
SymTabTy ReadSymbolTable(BinaryInfo &binary_info) {
  SymTabTy symtab;
  for (llvm::object::SymbolRef sr : binary_info.object_file->symbols()) {
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
absl::StatusOr<std::vector<BBAddrMap>> ReadBbAddrMap(BinaryInfo &binary_info) {
  auto *elf_object = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(
      binary_info.object_file.get());
  CHECK(elf_object);
  Expected<std::vector<BBAddrMap>> bb_addr_map = elf_object->readBBAddrMap();
  if (!bb_addr_map) {
    return absl::InternalError(
        llvm::formatv(
            "Failed to read the LLVM_BB_ADDR_MAP section from {0}: {1}.",
            binary_info.file_name, llvm::fmt_consume(bb_addr_map.takeError()))
            .str());
  }
  if (bb_addr_map->empty()) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "'%s' does not have a non-empty LLVM_BB_ADDR_MAP section.",
        binary_info.file_name));
  }
  return std::move(*bb_addr_map);
}

// Returns a map from BB-address-map function indexes to their names.
absl::flat_hash_map<int, llvm::SmallVector<llvm::StringRef, 3>>
SetFunctionIndexToNamesMap(const SymTabTy &symtab,
                           const std::vector<BBAddrMap> &bb_addr_map) {
  absl::flat_hash_map<int, llvm::SmallVector<llvm::StringRef, 3>>
      function_index_to_names;
  for (int i = 0; i != bb_addr_map.size(); ++i) {
    auto iter = symtab.find(bb_addr_map[i].getFunctionAddress());
    if (iter == symtab.end()) {
      LOG(WARNING) << "BB address map for function at "
                   << absl::StrCat(absl::Hex(bb_addr_map[i].getFunctionAddress()))
                   << " has no associated symbol table entry!";
      continue;
    }
    llvm::SmallVector<llvm::StringRef, 3> aliases;
    for (const llvm::object::SymbolRef sr : iter->second)
      aliases.push_back(llvm::cantFail(sr.getName()));
    if (!aliases.empty())
      function_index_to_names.insert({i, std::move(aliases)});
  }
  return function_index_to_names;
}
}  // namespace

std::optional<int>
PropellerWholeProgramInfo::FindBbHandleIndexUsingBinaryAddress(
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

std::unique_ptr<PropellerWholeProgramInfo> PropellerWholeProgramInfo::Create(
    const PropellerOptions &options, MultiStatusProvider *frontend_status) {
  return Create(options,
                std::make_unique<FilePerfDataProvider>(std::vector<std::string>(
                    options.perf_names().begin(), options.perf_names().end())),
                frontend_status);
}
std::unique_ptr<PropellerWholeProgramInfo> PropellerWholeProgramInfo::Create(
    const PropellerOptions &options,
    std::unique_ptr<PerfDataProvider> perf_data_provider,
    MultiStatusProvider *frontend_status) {
  DefaultStatusProvider *s1 = nullptr, *s2 = nullptr;
  if (frontend_status) {
    s1 =
        new DefaultStatusProvider("read binary and the bb-address-map section");
    s2 = new DefaultStatusProvider("map profiles to control flow graphs");
    frontend_status->AddStatusProvider(10, absl::WrapUnique(s1));
    frontend_status->AddStatusProvider(500, absl::WrapUnique(s2));
  }
  BinaryPerfInfo bpi;
  if (!PerfDataReader().SelectBinaryInfo(options.binary_name(),
                                         &bpi.binary_info))
    return nullptr;
  if (s1) s1->SetDone();

  // Do not use std::make_unique, ctor is not public.
  return std::unique_ptr<PropellerWholeProgramInfo>(
      new PropellerWholeProgramInfo(options, std::move(bpi),
                                    std::move(perf_data_provider), s2));
}

// Parse perf data file.
absl::StatusOr<LBRAggregation> PropellerWholeProgramInfo::ParsePerfData() {
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

  binary_perf_info_.ResetPerfInfo();
  while (true) {
    ASSIGN_OR_RETURN(std::optional<PerfDataProvider::BufferHandle> perf_data,
                     perf_data_provider_->GetNext());

    if (!perf_data.has_value()) break;

    std::string description = perf_data->description;
    LOG(INFO) << "Parsing " << description << " ...";
    if (!PerfDataReader().SelectPerfInfo(std::move(*perf_data), match_mmap_name,
                                         &binary_perf_info_)) {
      LOG(WARNING) << "Skipped profile " << description
                   << ", because reading file failed or no mmap found.";
      continue;
    }
    if (binary_perf_info_.binary_mmaps.empty()) {
      LOG(WARNING) << "Skipped profile " << description
                   << ", because no matching mmap found.";
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
      return absl::InvalidArgumentError(
          "--keep_frontend_intermediate_data is only valid for single profile "
          "file input.");
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
    return absl::FailedPreconditionError(
        "No perf file is parsed, cannot proceed.");
  }
  return lbr_aggregation;
}

absl::Status PropellerWholeProgramInfo::DoCreateCfgs(
    LBRAggregation &&lbr_aggregation,
    absl::btree_set<int> &&selected_functions) {
  // Temporary map from symbol ordinal -> CFGNode.
  absl::flat_hash_map<int, CFGNode *> node_map;
  // Temporary map from function index -> ControlFlowGraph.
  absl::btree_map<int, std::unique_ptr<ControlFlowGraph>> cfg_map;
  uint64_t ordinal = 0;
  for (int func_index : selected_functions) {
    const BBAddrMap &func_bb_addr_map = bb_addr_map_[func_index];
    CHECK(!func_bb_addr_map.getBBEntries().empty());
    auto cfg = std::make_unique<ControlFlowGraph>(
        function_index_to_names_map_[func_index]);
    cfg->CreateNodes(func_bb_addr_map, ordinal);
    CHECK_EQ(cfg->nodes().size(), func_bb_addr_map.getBBEntries().size());
    ordinal += cfg->nodes().size();
    stats_.nodes_created += cfg->nodes().size();
    // Setup mapping from symbol ordinals <-> nodes
    for (const std::unique_ptr<CFGNode> &node : cfg->nodes())
      node_map.insert({node->symbol_ordinal(), node.get()});
    cfg_map.emplace(func_index, std::move(cfg));
    ++stats_.cfgs_created;
  }
  if (!CreateEdges(lbr_aggregation, node_map))
    return absl::InternalError("Unable to create edges from LBR profile.");

  for (auto &[func_index, cfg] : cfg_map) {
    cfg->CalculateNodeFreqs();
    if (cfg->n_hot_landing_pads() != 0) ++stats_.cfgs_with_hot_landing_pads;
    cfgs_.insert(
        {function_index_to_names_map_[func_index].front(), std::move(cfg)});
  }

  // Release / cleanup.
  if (!options_.keep_frontend_intermediate_data()) {
    binary_perf_info_.binary_mmaps.clear();
    lbr_aggregation = LBRAggregation();
    // Release ownership and delete symbols and BBAddrMap entries.
    symtab_.clear();
    function_index_to_names_map_.clear();
    bb_addr_map_.clear();
  }
  return absl::OkStatus();
}

CFGEdge *PropellerWholeProgramInfo::InternalCreateEdge(
    int from_bb_ordinal, int to_bb_ordinal, uint64_t weight,
    CFGEdge::Kind edge_kind,
    const absl::flat_hash_map<int, CFGNode *> &tmp_node_map,
    absl::flat_hash_map<std::pair<int, int>, CFGEdge *> *tmp_edge_map) {
  CFGEdge *edge = nullptr;
  auto i = tmp_edge_map->find(std::make_pair(from_bb_ordinal, to_bb_ordinal));
  if (i != tmp_edge_map->end()) {
    edge = i->second;
    if (edge->kind() != edge_kind) {
      LOG(WARNING) << "Edges with same src and sink have different type: "
                   << CFGEdgeNameFormatter(edge) << " has type "
                   << CFGEdge::GetCfgEdgeKindString(edge_kind) << " and "
                   <<  CFGEdge::GetCfgEdgeKindString(edge->kind());
      ++stats_.edges_with_same_src_sink_but_different_type;
    }
    edge->IncrementWeight(weight);
  } else {
    auto from_ni = tmp_node_map.find(from_bb_ordinal),
         to_ni = tmp_node_map.find(to_bb_ordinal);
    if (from_ni == tmp_node_map.end() || to_ni == tmp_node_map.end())
      return nullptr;
    CFGNode *from_node = from_ni->second;
    CFGNode *to_node = to_ni->second;
    DCHECK(from_node && to_node);
    edge = from_node->cfg()->CreateEdge(from_node, to_node, weight, edge_kind);
    ++stats_.edges_created_by_kind[edge_kind];
    tmp_edge_map->emplace(std::piecewise_construct,
                          std::forward_as_tuple(from_bb_ordinal, to_bb_ordinal),
                          std::forward_as_tuple(edge));
  }
  stats_.total_edge_weight_by_kind[edge_kind] += weight;
  return edge;
}

// Create control flow graph edges from branch_counters_. For each address pair
// <from_addr, to_addr> in "branch_counters_", we translate it to <from_symbol,
// to_symbol> and by using tmp_node_map, we further translate it to <from_node,
// to_node>, and finally create a CFGEdge for such CFGNode pair.
bool PropellerWholeProgramInfo::CreateEdges(
    const LBRAggregation &lbr_aggregation,
    const absl::flat_hash_map<int, CFGNode *> &tmp_node_map) {
  // Temp map that records which CFGEdges are created, so we do not re-create
  // edges. Note this is necessary: although
  // "branch_counters_" have no duplicated <from_addr, to_addr> pairs, the
  // translated <from_bb, to_bb> may have duplicates.
  absl::flat_hash_map<std::pair<int, int>, CFGEdge *> tmp_edge_map;

  absl::flat_hash_map<std::pair<int, int>, uint64_t>
      tmp_bb_fallthrough_counters;

  uint64_t weight_on_dubious_edges = 0;
  uint64_t edges_recorded = 0;
  for (const typename BranchCountersTy::value_type &bcnt :
       lbr_aggregation.branch_counters) {
    ++edges_recorded;
    uint64_t from = bcnt.first.first;
    uint64_t to = bcnt.first.second;
    uint64_t weight = bcnt.second;
    std::optional<int> from_bb_index =
        FindBbHandleIndexUsingBinaryAddress(from, BranchDirection::kFrom);
    std::optional<int> to_bb_index =
        FindBbHandleIndexUsingBinaryAddress(to, BranchDirection::kTo);
    if (!to_bb_index.has_value()) continue;

    BbHandle to_bb_handle = bb_handles_[*to_bb_index];
    BbHandle from_bb_handle = from_bb_index.has_value()
                                  ? bb_handles_[*from_bb_index]
                                  : BbHandle(-1, -1);
    // This is to handle the case when a call is the last instr of a basicblock,
    // and a return to the beginning of the next basic block, change the to_sym
    // to the basic block just before and add fallthrough between the two
    // symbols. After this code executes, "to" can never be the beginning
    // of a basic block for returns.
    // We also account for returns from external library functions which happen
    // when from_sym is null.
    if ((!from_bb_index.has_value() || GetBBEntry(from_bb_handle).hasReturn() ||
         to_bb_handle.function_index != from_bb_handle.function_index) &&
        // Not a call
        GetFunctionEntry(to_bb_handle).getFunctionAddress() != to &&
        // Jump to the beginning of the basicblock
        to == GetAddress(to_bb_handle)) {
      if (to_bb_handle.bb_index != 0) {
        // Account for the fall-through between callSiteSym and toSym.
        tmp_bb_fallthrough_counters[{*to_bb_index - 1, *to_bb_index}] += weight;
        // Reassign to_bb to be the actuall callsite symbol entry.
        --*to_bb_index;
      } else {
        LOG(WARNING) << "*** Warning: Could not find the right "
                        "call site sym for : "
                     << GetName(to_bb_handle);
      }
    }
    if (!from_bb_index.has_value()) continue;
    if (!GetBBEntry(from_bb_handle).hasReturn() &&
        GetAddress(to_bb_handle) != to) {
      // Jump is not a return and its target is not the beginning of a function
      // or a basic block.
      weight_on_dubious_edges += weight;
    }

    CFGEdge::Kind edge_kind = CFGEdge::Kind::kBranchOrFallthough;
    if (GetFunctionEntry(to_bb_handle).getFunctionAddress() == to) {
      edge_kind = CFGEdge::Kind::kCall;
    } else if (to != GetAddress(to_bb_handle) ||
               GetBBEntry(from_bb_handle).hasReturn()) {
      edge_kind = CFGEdge::Kind::kRet;
    }
    InternalCreateEdge(from_bb_index.value(), to_bb_index.value(), weight,
                       edge_kind, tmp_node_map, &tmp_edge_map);
  }

  if (weight_on_dubious_edges /
          static_cast<double>(stats_.total_edge_weight_created()) >
      0.3) {
    LOG(ERROR) << "Too many jumps into middle of basic blocks detected, "
                  "probably because of source drift ("
               << CommaStyleNumberFormatter(weight_on_dubious_edges)
               << " out of "
               << CommaStyleNumberFormatter(stats_.total_edge_weight_created())
               << ").";
    return false;
  }

  if (stats_.total_edges_created() / static_cast<double>(edges_recorded) <
      0.0005) {
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
    const absl::flat_hash_map<int, CFGNode *> &tmp_node_map,
    absl::flat_hash_map<std::pair<int, int>, uint64_t>
        *tmp_bb_fallthrough_counters,
    absl::flat_hash_map<std::pair<int, int>, CFGEdge *> *tmp_edge_map) {
  for (auto &i : lbr_aggregation.fallthrough_counters) {
    const uint64_t cnt = i.second;
    // A fallthrough from A to B implies a branch to A followed by a branch
    // from B. Therefore we respectively use BranchDirection::kTo and
    // BranchDirection::kFrom for A and B when calling
    // `FindBbHandleIndexUsingBinaryAddress` to find their associated blocks.
    auto from_index = FindBbHandleIndexUsingBinaryAddress(i.first.first,
                                                          BranchDirection::kTo);
    auto to_index = FindBbHandleIndexUsingBinaryAddress(i.first.second,
                                                        BranchDirection::kFrom);
    if (from_index && to_index)
      (*tmp_bb_fallthrough_counters)[{*from_index, *to_index}] += cnt;
  }

  for (auto &i : *tmp_bb_fallthrough_counters) {
    int fallthrough_from = i.first.first, fallthrough_to = i.first.second;
    uint64_t weight = i.second;
    if (fallthrough_from == fallthrough_to ||
        !CanFallThrough(fallthrough_from, fallthrough_to))
      continue;

    for (int sym = fallthrough_from; sym <= fallthrough_to - 1; ++sym) {
      auto *fallthrough_edge = InternalCreateEdge(
          sym, sym + 1, weight, CFGEdge::Kind::kBranchOrFallthough,
          tmp_node_map, tmp_edge_map);
      if (!fallthrough_edge) break;
    }
  }
}

bool PropellerWholeProgramInfo::CanFallThrough(int from, int to) {
  if (from == to) return true;
  BbHandle from_bb = bb_handles_[from];
  BbHandle to_bb = bb_handles_[to];
  if (GetAddress(from_bb) > GetAddress(to_bb)) {
    LOG(WARNING) << "*** Internal error: fallthrough path start address is "
                    "larger than end address. ***";
    return false;
  }
  if (!GetBBEntry(from_bb).canFallThrough()) {
    LOG(WARNING) << "*** Skipping non-fallthrough ***" << GetName(from_bb);
    return false;
  }

  if (from_bb.function_index != to_bb.function_index) {
    LOG(ERROR)
        << "fallthrough (" << GetName(from_bb) << " -> " << GetName(to_bb)
        << ") does not start and end within the same function.";
    return false;
  }
  for (int i = from_bb.bb_index; i != to_bb.bb_index; ++i) {
    BbHandle bb_sym(from_bb.function_index, i);
    // (b/62827958) Sometimes LBR contains duplicate entries in the beginning
    // of the stack which may result in false fallthrough paths. We discard
    // the fallthrough path if any intermediate block (except the destination
    // block) does not fall through (source block is checked before entering
    // this loop).
    if (!GetBBEntry(bb_sym).canFallThrough()) {
      LOG(WARNING) << "*** Skipping non-fallthrough ***" << GetName(bb_sym);
      return false;
    }
  }
  // Warn about unusually-long fallthroughs.
  if (to - from >= 200) {
    LOG(WARNING) << "More than 200 BBs along fallthrough (" << GetName(from_bb)
                 << " -> " << GetName(to_bb) << "): " << std::dec
                 << to - from + 1 << " BBs.";
  }
  return true;
}

// For each lbr record addr1->addr2, find function1/2 that contain addr1/addr2
// and add function1/2's index into the returned set.
absl::btree_set<int> PropellerWholeProgramInfo::CalculateHotFunctions(
    const LBRAggregation &lbr_aggregation) {
  absl::btree_set<int> hot_functions;
  auto add_to_hot_functions = [this, &hot_functions](uint64_t binary_address) {
    auto it =
        absl::c_upper_bound(bb_addr_map_, binary_address,
                            [](uint64_t addr, const BBAddrMap &func_entry) {
                              return addr < func_entry.getFunctionAddress();
                            });
    if (it == bb_addr_map_.begin())
      return;
    it = std::prev(it);
    // We know the address is bigger than or equal to the function address. Make
    // sure that it doesn't point beyond the last basic block.
    if (binary_address >= it->getFunctionAddress() +
                              it->getBBEntries().back().Offset +
                              it->getBBEntries().back().Size)
      return;
    hot_functions.insert(it - bb_addr_map_.begin());
  };
  for (const auto &bcnt : lbr_aggregation.branch_counters) {
    add_to_hot_functions(bcnt.first.first);
    add_to_hot_functions(bcnt.first.second);
  }
  stats_.hot_functions = hot_functions.size();
  return hot_functions;
}

void PropellerWholeProgramInfo::DropNonSelectedFunctions(
    const absl::btree_set<int> &selected_functions) {
  for (int i = 0; i != bb_addr_map_.size(); ++i) {
    if (selected_functions.contains(i))
      continue;
    bb_addr_map_[i].BBRanges.clear();
    bb_addr_map_[i].BBRanges.shrink_to_fit();
    if (!options_.keep_frontend_intermediate_data())
      symtab_.erase(bb_addr_map_[i].getFunctionAddress());
  }
}

void PropellerWholeProgramInfo::FilterNoNameFunctions(
    absl::btree_set<int> &selected_functions) const {
  for (auto it = selected_functions.begin(); it != selected_functions.end();) {
    if (!function_index_to_names_map_.contains(*it)) {
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

void PropellerWholeProgramInfo::FilterNonTextFunctions(
    absl::btree_set<int> &selected_functions) const {
  for (auto func_it = selected_functions.begin();
       func_it != selected_functions.end();) {
    int function_index = *func_it;
    llvm::object::SymbolRef symbol_ref =
        symtab_.at(bb_addr_map_[function_index].getFunctionAddress()).front();
    StringRef section_name =
        llvm::cantFail(llvm::cantFail(symbol_ref.getSection())->getName());
    if (section_name.startswith(".text")) {
      ++func_it;
    } else {
      LOG(WARNING)
          << "Skipped symbol in none '.text.*' section '" << section_name.str()
          << "': "
          << function_index_to_names_map_.at(function_index).front().str();
      func_it = selected_functions.erase(func_it);
    }
  }
}

// Without '-funique-internal-linkage-names', if multiple functions have the
// same name, even though we can correctly map their profiles, we cannot apply
// those profiles back to their object files.
// This function removes all such functions which have the same name as other
// functions in the binary.
int PropellerWholeProgramInfo::FilterDuplicateNameFunctions(
    absl::btree_set<int> &selected_functions) const {
  int duplicate_symbols = 0;
  absl::flat_hash_map<StringRef, std::vector<int>> name_to_function_index;
  for (int func_index : selected_functions) {
    for (StringRef name : function_index_to_names_map_.at(func_index))
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
        return absl::c_equal(func_addr_map.getBBEntries(), bb_addr_map_[i].getBBEntries(),
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

absl::btree_set<int> PropellerWholeProgramInfo::SelectFunctions(
    CfgCreationMode cfg_creation_mode, const LBRAggregation *lbr_aggregation) {
  absl::btree_set<int> selected_functions;

  if (cfg_creation_mode == CfgCreationMode::kOnlyHotFunctions) {
    CHECK_NE(lbr_aggregation, nullptr);
    selected_functions = CalculateHotFunctions(*lbr_aggregation);
  } else {
    for (int i = 0; i != bb_addr_map_.size(); ++i) selected_functions.insert(i);
  }

  FilterNoNameFunctions(selected_functions);
  FilterNonTextFunctions(selected_functions);
  stats_.duplicate_symbols += FilterDuplicateNameFunctions(selected_functions);
  DropNonSelectedFunctions(selected_functions);

  std::optional<uint64_t> last_function_address;
  for (int function_index : selected_functions) {
    const auto &function_bb_addr_map = bb_addr_map_[function_index];
    if (last_function_address.has_value())
      CHECK_GT(function_bb_addr_map.getFunctionAddress(),
               *last_function_address);
    for (int bb_index = 0;
         bb_index != function_bb_addr_map.getBBEntries().size(); ++bb_index)
      bb_handles_.push_back({function_index, bb_index});
    last_function_address = function_bb_addr_map.getFunctionAddress();
  }

  return selected_functions;
}

absl::Status PropellerWholeProgramInfo::ReadBinaryInfo() {
  BinaryInfo &binary_info = binary_perf_info_.binary_info;
  LOG(INFO) << "Started reading the binary info from: "
            << binary_info.file_name;
  symtab_ = ReadSymbolTable(binary_info);
  ASSIGN_OR_RETURN(bb_addr_map_, ReadBbAddrMap(binary_info));
  function_index_to_names_map_ =
      SetFunctionIndexToNamesMap(symtab_, bb_addr_map_);
  stats_.bbaddrmap_function_does_not_have_symtab_entry +=
      bb_addr_map_.size() - function_index_to_names_map_.size();
  LOG(INFO) << "Finished reading the binary info from: "
            << binary_info.file_name;
  return absl::OkStatus();
}

// "CreateCfgs" steps:
//   1.a ReadSymbolTable
//   1.b ReadBbAddrMap
//   1.c ParsePerfData
//   2. CalculateHotFunctions or mark all functions as hot
//   3. DoCreateCfgs
absl::Status PropellerWholeProgramInfo::CreateCfgs(
    CfgCreationMode cfg_creation_mode) {
  absl::Status read_binary_info_status;
  std::thread read_binary_info_thread([this, &read_binary_info_status]() {
    read_binary_info_status = ReadBinaryInfo();
  });

  absl::StatusOr<LBRAggregation> lbr_aggregation = ParsePerfData();

  read_binary_info_thread.join();
  if (!read_binary_info_status.ok())
    return read_binary_info_status;

  if (status_provider_) status_provider_->SetProgress(20);

  if (cfg_creation_mode == CfgCreationMode::kOnlyHotFunctions &&
      !lbr_aggregation.ok()) {
    return lbr_aggregation.status();
  }

  absl::btree_set<int> selected_functions = SelectFunctions(
      cfg_creation_mode, lbr_aggregation.ok() ? &*lbr_aggregation : nullptr);
  if (status_provider_) status_provider_->SetProgress(50);

  // This must be done only after both PopulateSymbolMaps and ParsePerfData
  // finish. Note: opt_lbr_aggregation is released afterwards.
  absl::Status status =
      DoCreateCfgs(std::move(*lbr_aggregation), std::move(selected_functions));
  if (status_provider_) status_provider_->SetDone();
  return status;
}
}  // namespace devtools_crosstool_autofdo
