#ifndef AUTOFDO_LLVM_PROPELLER_WHOLE_PROGRAM_INFO_H_
#define AUTOFDO_LLVM_PROPELLER_WHOLE_PROGRAM_INFO_H_

#if defined(HAVE_LLVM)

#include <future>  // NOLINT(build/c++11)
#include <list>
#include <map>
#include <memory>
#include <vector>

#include "llvm_propeller_abstract_whole_program_info.h"
#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "perfdata_reader.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

namespace devtools_crosstool_autofdo {

class PropellerWholeProgramInfo : public AbstractPropellerWholeProgramInfo {
 public:
  static const uint64_t kInvalidAddress = static_cast<uint64_t>(-1);

  static constexpr char kBbAddrMapSectionName[] = ".llvm_bb_addr_map";

  // Address -> a set of symbols which point to this address. This takes
  // ownership of SymbolEntries.
  using AddressMapTy =
      std::map<uint64_t, llvm::SmallVector<std::unique_ptr<SymbolEntry>, 2>>;

  // All SymbolEntries in the vector (which is never empty) are bb symbols.
  // We don't store FuncPtr directly in the map, but we can always get it from:
  // bb_addr_map_[func_name].front()->func_ptr. And BBSymbol with index "x" is
  // stored in bb_addr_map_[BBSymbol->func_ptr->name][x].
  using BbAddrMapTy = std::map<llvm::StringRef, std::vector<SymbolEntry *>>;

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
      const PropellerOptions &options);

  // TODO(b/160191690): properly arrange public/protected/private sections.
 private:
  PropellerWholeProgramInfo(
      const PropellerOptions &options,
      BinaryPerfInfo &&bpi,
      llvm::object::SectionRef bb_addr_map_section)
      : AbstractPropellerWholeProgramInfo(options),
        binary_perf_info_(std::move(bpi)),
        bb_addr_map_section_(bb_addr_map_section) {}

 public:
  ~PropellerWholeProgramInfo() override {}

  // Whether binary is position independe.
  bool binary_is_pie() const { return binary_perf_info_.binary_info.is_pie; }

  // Getters.
  const string binary_build_id() const {
    return binary_perf_info_.binary_info.build_id;
  }

  const PropellerOptions &options() const { return options_; }

  const BinaryPerfInfo &binray_perf_info() const { return binary_perf_info_; }

  const BbAddrMapTy &bb_addr_map() const { return bb_addr_map_; }

  const AddressMapTy &address_map() const { return address_map_; }

  const BinaryMMaps &binary_mmaps() const {
    return binary_perf_info_.binary_mmaps;
  }

  const PerfDataReader &perf_data_reader() const { return perf_data_reader_; }

  const SymTabTy &symtab() const { return symtab_; }

  const SymbolEntry *FindSymbolUsingBinaryAddress(uint64_t symbol_addr) const {
    auto i = address_map_.upper_bound(symbol_addr);
    if (i == address_map_.begin()) return nullptr;
    i = std::prev(i);
    // This is similar to "ContainsAnotherSymbol" (but instead of comparing 2
    // symbols, this compares an address with a symbol). The range of valid
    // addressed mapped to a Symbol is [s->addr, s->addr + s->size].

    // Here is an example:
    //   s1: addr = 100, size = 10
    //   s2: addr = 110, size = 20
    //   s3: addr = 130, size = 10

    // When searching for symbol that contains 110, we first get
    // i=upper_bound(110), that points to s3, then swith to prev(i), that points
    // to s2, and s2->addr<=110 && 110<=s2->addr+s2->size, so s2 is the result.
    // Even if 110 is the end address of "s1", we do not return s1, which is
    // wrong.
    //
    // A note about the size of i->second : i->second is of type
    // SmallVector<unique_ptr<SymbolEntry>>, which stores a set of symbols that
    // start at the same address (and the address value is used in i->first as
    // map key). Most of the time (99%+), the size of i->second is 1.
    SymbolEntry *func_sym = nullptr;
    SymbolEntry *bb_sym = nullptr;
    for (auto &s : i->second) {
      // TODO(b/166130806): properly handle 2 bb symbols, 1 zero-sized and 1
      // non-zero-sied bb, this may need additional metadata in bbaddrmap.
      if (s->addr <= symbol_addr && symbol_addr < s->addr + s->size + 1) {
        if (s->IsFunction() && !func_sym) func_sym = s.get();
        if (!s->IsFunction() && !bb_sym) bb_sym = s.get();
      }
    }
    if (func_sym && !bb_sym)
      return func_sym;
    // For bbaddrmap workflow, func_sym and entry bb_sym always share the same
    // address, in this case, return the entry bb_sym.
    return bb_sym;
  }

  const SymbolEntry *FindSymbolUsingRuntimeAddress(
      uint64_t pid, uint64_t runtime_addr) const {
    uint64_t symbol_address = perf_data_reader_.RuntimeAddressToBinaryAddress(
        pid, runtime_addr, binary_perf_info_);
    if (symbol_address == kInvalidAddress) return nullptr;
    return FindSymbolUsingBinaryAddress(symbol_address);
  }

  llvm::Optional<LBRAggregation> ParsePerfData();

  // Create a funcsym, insert it into address_map_. "func_bb_num" is the total
  // number of basic blocks this function has. "ordinal" must not be 0.
  SymbolEntry *CreateFuncSymbolEntry(uint64_t ordinal,
                                     llvm::StringRef func_name,
                                     SymbolEntry::AliasesTy aliases,
                                     int func_bb_num, uint64_t address,
                                     uint64_t size);

  // Create a bbsym, insert it into bb_addr_map_ and address_map_. "bbidx" is
  // the basic block index of the function, starting from 0. "ordinal" must not
  // be 0 and "parent_func" must point to a valid functon SymbolEntry.
  SymbolEntry *CreateBbSymbolEntry(uint64_t ordinal, SymbolEntry *parent_func,
                                   int bb_index, uint64_t address,
                                   uint64_t size, uint32_t metadata);

 public:
  // Get file content and set up binary_is_pie_ flag.
  bool InitBinaryFile();

  // Reading symbols info from .bb_addr_map section. The second overloaded
  // version is used in multi-threaded mode. The first version is used
  // in unit tests.
  bool PopulateSymbolMap();
  bool PopulateSymbolMapWithPromise(std::promise<bool> result_promise);

  // Read function symbols from elf's symbol table.
  void ReadSymbolTable();

  // Reads ".llvm_bb_addr_map" section. Only returns true if all bytes are
  // parsed correctly.
  bool ReadBbAddrMapSection();

  bool WriteSymbolsToProtobuf();

  // We select mmap events from perfdata file by comparing the mmap event's
  // binary name against one of the following:
  //   1. name from --mmap_name
  //   2. if no 1 is specified on the command line, we use
  //      this->binary_mmap_name_.
  //   3. if no 1 and no 2, use this->options_.binary_name.
  bool SelectMMaps(const quipper::PerfParser &parser,
                   const std::string &perf_name,
                   const std::string &buildid_name);

  bool CreateCfgs() override;

  bool DoCreateCfgs(LBRAggregation &&lbr_aggregation);

  // Helper method.
  CFGEdge *InternalCreateEdge(
      const SymbolEntry *from_sym, const SymbolEntry *to_sym, uint64_t weight,
      CFGEdge::Kind edge_kind,
      const std::map<const SymbolEntry *, CFGNode *, SymbolPtrComparator>
          &tmp_node_map,
      std::map<SymbolPtrPair, CFGEdge *, SymbolPtrPairComparator>
          *tmp_edge_map);

  // Helper method that creates edges and assign edge weights using
  // branch_counters_. Details in .cc.
  bool CreateEdges(const LBRAggregation &lbr_aggregation,
                   const std::map<const SymbolEntry *, CFGNode *,
                                  SymbolPtrComparator> &tmp_node_map);

  // Helper method that creates edges and assign edge weights using
  // branch_counters_. Details in .cc.
  void CreateFallthroughs(
      const LBRAggregation &lbr_aggregation,
      const std::map<const SymbolEntry *, CFGNode *, SymbolPtrComparator>
          &tmp_node_map,
      std::map<SymbolPtrPair, uint64_t, SymbolPtrPairComparator>
          *tmp_bb_fallthrough_counters,
      std::map<SymbolPtrPair, CFGEdge *, SymbolPtrPairComparator>
          *tmp_edge_map);

  // Compute fallthrough BBs for "from" -> "to", and place them in "path".
  // ("from" and "to" are excluded). Details in .cc.
  bool CalculateFallthroughBBs(const SymbolEntry &from, const SymbolEntry &to,
                               std::vector<const SymbolEntry *> *path);

  // Returns the next available ordinal.
  uint64_t AllocateSymbolOrdinal() { return ++last_symbol_ordinal_; }

 private:
  // Handler to PerfDataReader handler.
  PerfDataReader perf_data_reader_;
  BinaryPerfInfo binary_perf_info_;
  // Very rarely we create strings by modifying the strings from elf content,
  // these modified strings are not part of "binary_file_content_", so we heap
  // allocate them and make StringRefs to use them.
  struct Deleter {
    void operator()(char *c) { delete[] c; }
  };
  std::list<std::unique_ptr<char, Deleter>> string_vault_;
  // See AddressMapTy.
  AddressMapTy address_map_;
  // See BbAddrMapTy.
  BbAddrMapTy bb_addr_map_;

  // Handle to .llvm_bb_addr_map section.
  llvm::object::SectionRef bb_addr_map_section_;
  // See SymTabTy definition. Deleted after "CreateCfgs()".
  SymTabTy symtab_;

  // Each symbol has a unique ordinal. This variable stores the ordinal assigned
  // the last symbol.
  uint64_t last_symbol_ordinal_ = 0;
};
}  // namespace devtools_crosstool_autofdo

#endif
#endif  // AUTOFDO_LLVM_PROPELLER_WHOLE_PROGRAM_INFO_H_
