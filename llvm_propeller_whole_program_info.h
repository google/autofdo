#ifndef AUTOFDO_LLVM_PROPELLER_WHOLE_PROGRAM_INFO_H_
#define AUTOFDO_LLVM_PROPELLER_WHOLE_PROGRAM_INFO_H_

#if defined(HAVE_LLVM)

#include <future>  // NOLINT(build/c++11)
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_statistics.h"
#include "perfdata_reader.h"
#include "status_provider.h"
#include "third_party/abseil/absl/container/btree_set.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"

namespace devtools_crosstool_autofdo {

// A struct representing one basic block entry in the BB address map.
struct BbHandle {
  BbHandle(int fi, int bbi) : function_index(fi), bb_index(bbi) {}
  int function_index, bb_index;
};

enum class BranchDirection { kFrom, kTo };

class PropellerWholeProgramInfo : public AbstractPropellerWholeProgramInfo {
 public:
  static const uint64_t kInvalidAddress = static_cast<uint64_t>(-1);

  static constexpr char kBbAddrMapSectionName[] = ".llvm_bb_addr_map";

  // Non-zero sized function symbols from elf symbol table, indexed by
  // symbol address. Multiple function symbols may exist on the same address.
  // Ideally we should store ELFSymbolRef instead of SymbolRef, however, it is
  // not a value object, we cannot copy or move a ELFSymbolRef.
  using SymTabTy =
      std::map<uint64_t, llvm::SmallVector<llvm::object::SymbolRef, 2>>;

  // <from_address, to_address> -> branch counter.
  // Note all addresses are binary addresses, not runtime addresses.
  using BranchCountersTy = std::map<std::pair<uint64_t, uint64_t>, uint64_t>;

  // <fallthrough_from, fallthrough_to> -> fallthrough counter.
  // Note all addresses are symbol address, not virtual addresses.
  using FallthroughCountersTy =
      std::map<std::pair<uint64_t, uint64_t>, uint64_t>;

  static std::unique_ptr<PropellerWholeProgramInfo> Create(
      const PropellerOptions &options,
      MultiStatusProvider *status  = nullptr);
  // Like above, but `opts.perf_names` is ignored and `perf_data_provider` is
  // used instead.
  static std::unique_ptr<PropellerWholeProgramInfo> Create(
      const PropellerOptions &options,
      std::unique_ptr<PerfDataProvider> perf_data_provider,
      MultiStatusProvider *status  = nullptr);

 public:
  ~PropellerWholeProgramInfo() override {}

  // Whether binary is position independe.
  bool binary_is_pie() const { return binary_perf_info_.binary_info.is_pie; }

  // Getters.
  const string binary_build_id() const {
    return binary_perf_info_.binary_info.build_id;
  }

  const PropellerOptions &options() const { return options_; }

  const BinaryMMaps &binary_mmaps() const {
    return binary_perf_info_.binary_mmaps;
  }

  const PerfDataReader &perf_data_reader() const { return perf_data_reader_; }

  const SymTabTy &symtab() const { return symtab_; }

  const std::vector<llvm::object::BBAddrMap> &bb_addr_map() const {
    return bb_addr_map_;
  }

  const std::vector<BbHandle> &bb_handles() const { return bb_handles_; }

  const absl::flat_hash_map<int, llvm::SmallVector<llvm::StringRef, 3>>
      &function_index_to_names_map() const {
    return function_index_to_names_map_;
  }

  // Returns the `bb_handles_` index associated with the binary address
  // `address` given a branch from/to this address based on `direction`.
  // It returns nullopt if the no `bb_handles_` index can be mapped.
  // When zero-sized blocks exist, multiple blocks could be mapped to the
  // address. We make this decision based the given branch `direction` for the
  // address. For example, consider the following range of blocks from two
  // functions foo and bar.
  // ...
  // 0x10:  <foo.5> [size: 0x6]
  // 0x16:  <foo.6> [size: 0x4]
  // 0x1a:  <foo.7> [size: 0x0]
  // 0x1a:  <foo.8> [size: 0x0]
  // 0x1a:  <foo.9> [size: 0x6]
  // 0x20:  <foo.10> [size: 0x0]
  // 0x20:  <bar.0> [size: 0x10]
  // ...
  // 1- address=0x12, direction=kFrom/kTo -> returns foo.5
  //    This is the simple case where address falls within the block.
  // 2- address=0x16, direction=kFrom/kTo -> returns <foo.6>
  //    Address falls at the beginning of <foo.6> and there are no empty blocks
  //    at the same address.
  // 3- address=0x1a, direction=kTo -> returns <foo.7>
  //    <foo.7>, <foo.8>, and <foo.9> all start at this address. We return the
  //    first empty block, which falls through to the rest. In this case <foo.7>
  // 4- address=0x1a, direction=kFrom -> returns <foo.9>.
  //    We cannot have a branch "from" an empty block. So we return the single
  //    non-empty block at this address.
  // 5- address=0x20, direction=kTo/kFrom -> returns <bar.0>
  //    Even though <foo.10> is an empty block at the same address as <bar.0>,
  //    it won't be considered because it's from a different function.
  std::optional<int> FindBbHandleIndexUsingBinaryAddress(
      uint64_t address, BranchDirection direction) const;

  absl::StatusOr<LBRAggregation> ParsePerfData();

  // Reads bb_addr_map_ and symtab_ from the binary. Also constructs
  // the function_index_to_names_ map.
  absl::Status ReadBinaryInfo();

  // Creates the CFGs. Only creates CFGs for hot functions if
  // `cfg_creation_mode==kOnlyHotFunctions`.
  absl::Status CreateCfgs(CfgCreationMode cfg_creation_mode) override;

  // Returns a list of hot functions based on profiles. This must be called
  // after "ReadSymbolTable()", which initializes symtab_ and "ParsePerfData()"
  // which provides "lbr_aggregation". The returned btree_set specifies the
  // hot functions by their index in `bb_addr_map()`.
  absl::btree_set<int> CalculateHotFunctions(
      const LBRAggregation &lbr_aggregation);

  // Removes unwanted functions from the BB address map and symbol table, and
  // returns the remaining functions by their indexes in `bb_addr_map()`.
  // This function removes all non-text functions, functions without associated
  // names, and those with duplicate names. If
  // `cfg_creation_mode=kOnlyHotFunctions` it also captures and removes all cold
  // functions.
  // `lbr_aggregation` may only be null if `cfg_creation_mode=kAllFunctions`.
  absl::btree_set<int> SelectFunctions(CfgCreationMode cfg_creation_mode,
                                       const LBRAggregation *lbr_aggregation);

  // Creates profile CFGs for functions in `selected_functions`, using the LBR
  // profile in `lbr_aggregation`. `selected functions` must be a set of
  // indexes into `bb_addr_map_`.
  absl::Status DoCreateCfgs(LBRAggregation &&lbr_aggregation,
                            absl::btree_set<int> &&selected_functions);

  // Removes all functions that are not included (selected) in the
  // `selected_functions` set. Clears their associated BB entries from
  // `bb_addr_map_` and also removes their associated entries from `symtab_`.
  void DropNonSelectedFunctions(const absl::btree_set<int> &selected_functions);

  // Returns the full function's BB address map associated with the given
  // `bb_handle`.
  const llvm::object::BBAddrMap &GetFunctionEntry(BbHandle bb_handle) const {
    return bb_addr_map_.at(bb_handle.function_index);
  }

  // Returns the basic block's address map entry associated with the given
  // `bb_handle`.
  const llvm::object::BBAddrMap::BBEntry &GetBBEntry(BbHandle bb_handle) const {
    return bb_addr_map_.at(bb_handle.function_index)
        .BBEntries.at(bb_handle.bb_index);
  }

  uint64_t GetAddress(BbHandle bb_handle) const {
    return GetFunctionEntry(bb_handle).Addr + GetBBEntry(bb_handle).Offset;
  }

  // Returns the name associated with the given `bb_handle`.
  std::string GetName(BbHandle bb_handle) const {
    auto &aliases = function_index_to_names_map_.at(bb_handle.function_index);
    std::string func_name =
        aliases.empty()
            ? absl::StrCat("0x", absl::Hex(GetFunctionEntry(bb_handle).Addr))
            : aliases.front().str();
    return absl::StrCat(func_name, ":", bb_handle.bb_index);
  }

 private:
  PropellerWholeProgramInfo(
      const PropellerOptions &options, BinaryPerfInfo &&bpi,
      std::unique_ptr<PerfDataProvider> perf_data_provider,
      DefaultStatusProvider *status_provider)
      : AbstractPropellerWholeProgramInfo(options),
        binary_perf_info_(std::move(bpi)),
        perf_data_provider_(std::move(perf_data_provider)),
        status_provider_(status_provider) {}

  // Removes all functions without associated symbol names from the given
  // function indices.
  void FilterNoNameFunctions(absl::btree_set<int> &selected_functions) const;

  // This removes all functions in non-text sections from the specified set of
  // function indices.
  void FilterNonTextFunctions(absl::btree_set<int> &selected_functions) const;

  // Removes all functions with duplicate names from the specified function
  // indices. Must be called after `FilterNoNameFunctions`.
  int FilterDuplicateNameFunctions(
      absl::btree_set<int> &selected_functions) const;

  // Creates and returns an edge from `from_bb` to `to_bb` (specified by their
  // symbol_ordinal) with the given `weight` and `edge_kind` and associates it
  // to the corresponding nodes specified by `tmp_node_map`. Finally inserts the
  // edge into `tmp_edge_map` with the key being the pair `{from_bb, to_bb}`.
  CFGEdge *InternalCreateEdge(
      int from_bb, int to_bb, uint64_t weight, CFGEdge::Kind edge_kind,
      const absl::flat_hash_map<int, CFGNode *> &tmp_node_map,
      absl::flat_hash_map<std::pair<int, int>, CFGEdge *> *tmp_edge_map);

  // Helper method that creates edges and assign edge weights using
  // branch_counters_. Details in .cc.
  bool CreateEdges(const LBRAggregation &lbr_aggregation,
                   const absl::flat_hash_map<int, CFGNode *> &tmp_node_map);

  // Helper method that creates edges and assign edge weights using
  // branch_counters_. Details in .cc.
  void CreateFallthroughs(
      const LBRAggregation &lbr_aggregation,
      const absl::flat_hash_map<int, CFGNode *> &tmp_node_map,
      absl::flat_hash_map<std::pair<int, int>, uint64_t>
          *tmp_bb_fallthrough_counters,
      absl::flat_hash_map<std::pair<int, int>, CFGEdge *> *tmp_edge_map);

  // Returns whether the `from` basic block can fallthrough to the `to` basic
  // block. `from` and `to` should be indices into the `bb_handles()` vector.
  bool CanFallThrough(int from, int to);

  // Handler to PerfDataReader handler.
  PerfDataReader perf_data_reader_;
  BinaryPerfInfo binary_perf_info_;
  std::unique_ptr<PerfDataProvider> perf_data_provider_;
  // Very rarely we create strings by modifying the strings from elf content,
  // these modified strings are not part of "binary_file_content_", so we heap
  // allocate them and make StringRefs to use them.
  struct Deleter {
    void operator()(char *c) { delete[] c; }
  };
  std::list<std::unique_ptr<char, Deleter>> string_vault_;

  // BB handles for all basic blocks of the selected functions. BB handles are
  // ordered in increasing order of their addresses. Thus every function's
  // BB handles are consecutive and in the order of their addresses. e.g.,
  // <func_idx_1, 0>
  // <func_idx_1, 1>
  // ...
  // <func_idx_1, n_1>
  // <func_idx_2, 0>
  // ...
  // <func_idx_2, n_2>
  // ...
  std::vector<BbHandle> bb_handles_;

  // Handle to .llvm_bb_addr_map section.
  std::vector<llvm::object::BBAddrMap> bb_addr_map_;
  // See SymTabTy definition. Deleted after "CreateCfgs()".
  SymTabTy symtab_;

  absl::flat_hash_map<int, llvm::SmallVector<llvm::StringRef, 3>>
      function_index_to_names_map_;

  // Each symbol has a unique ordinal. This variable stores the ordinal assigned
  // the last symbol.
  uint64_t last_symbol_ordinal_ = 0;

  DefaultStatusProvider *status_provider_ = nullptr;
};
}  // namespace devtools_crosstool_autofdo

#endif
#endif  // AUTOFDO_LLVM_PROPELLER_WHOLE_PROGRAM_INFO_H_
