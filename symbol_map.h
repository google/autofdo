// Class to represent the symbol map. The symbol map is a map from
// symbol names to the symbol class.
// This class is thread-safe.

#ifndef AUTOFDO_SYMBOL_MAP_H_
#define AUTOFDO_SYMBOL_MAP_H_
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/macros.h"
#include "base/logging.h"
#include "addr2line.h"
#include "source_info.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/container/node_hash_map.h"
#include "third_party/abseil/absl/flags/declare.h"
#include "third_party/abseil/absl/strings/string_view.h"

#if defined(HAVE_LLVM)
#include "llvm/ADT/StringSet.h"
#endif
// Macros from gcc (profile.c)
#define NUM_GCOV_WORKING_SETS 128
#define WORKING_SET_INSN_PER_BB 10

// Whether to use discriminator encoding.
ABSL_DECLARE_FLAG(bool, use_discriminator_encoding);

#if defined(HAVE_LLVM)
// Whether to use FS discriminator.
ABSL_DECLARE_FLAG(bool, use_fs_discriminator);
// Whether to only use base discriminator in fsprofile.
ABSL_DECLARE_FLAG(bool, use_base_only_in_fs_discriminator);
#endif

ABSL_DECLARE_FLAG(int, inline_instances_at_same_loc_cutoff);

namespace devtools_crosstool_autofdo {

typedef absl::btree_map<std::string, uint64_t> CallTargetCountMap;
typedef std::pair<std::string, uint64_t> TargetCountPair;
typedef std::vector<TargetCountPair> TargetCountPairs;

class Addr2line;

/* Struct from gcc (basic-block.h).
   Working set size statistics for a given percentage of the entire
   profile (sum_all from the counter summary).  */
struct gcov_working_set_info {
 public:
  gcov_working_set_info() : num_counters(0), min_counter(0) {}
  /* Number of hot counters included in this working set.  */
  uint32_t num_counters;
  /* Smallest counter included in this working set.  */
  uint64_t min_counter;
};

// Returns a sorted vector of target_count pairs. target_counts is a pointer
// to an empty vector in which the output will be stored.
// Sorting is based on count in descending order.
void GetSortedTargetCountPairs(const CallTargetCountMap &call_target_count_map,
                               TargetCountPairs *target_counts);

// The miminal total samples for a outline symbol to be emitted to the profile.
const int64_t kMinSamples = 10;

// Represents profile information of a given source.
class ProfileInfo {
 public:
  ProfileInfo() : count(0), num_inst(0) {}
  ProfileInfo &operator+=(const ProfileInfo &other);

  uint64_t count;
  uint64_t num_inst;
  CallTargetCountMap target_map;
};

// Map from source stack to profile,
// TODO(dehao): deprecate this when old profile format is deprecated.
typedef std::map<const SourceStack, ProfileInfo> SourceStackCountMap;

// Map from a source location (represented by offset+discriminator) to profile.
typedef std::map<uint64_t, ProfileInfo> PositionCountMap;

struct Callsite {
  uint64_t location;
  const char *callee_name;
};
struct CallsiteHash {
  size_t operator()(const Callsite &callsite) const {
    return callsite.location;
  }
};
struct CallsiteEqual {
  bool operator()(const Callsite& c1, const Callsite& c2) const {
    if (c1.location != c2.location)
      return false;
    if (c1.callee_name == nullptr || c2.callee_name == nullptr)
      return c1.callee_name == c2.callee_name;
    return strcmp(c1.callee_name, c2.callee_name) == 0;
  }
};
struct CallsiteLessThan {
  bool operator()(const Callsite &c1, const Callsite &c2) const {
    if (c1.location != c2.location) return c1.location < c2.location;
    if (c1.callee_name == nullptr || c2.callee_name == nullptr)
      return c1.callee_name == nullptr && c2.callee_name != nullptr;
    return strcmp(c1.callee_name, c2.callee_name) < 0;
  }
};
class Symbol;
class SymbolMap;
// Map from a callsite to the callee symbol.
// Requires stability of pointers to value_type after insertion.
typedef absl::node_hash_map<Callsite, Symbol *, CallsiteHash, CallsiteEqual>
    CallsiteMap;
// Maps function names to symbols. Symbols are not owned and multiple names can
// map to the same symbol.
// Container must ensure iterator validity after insertion.
typedef std::map<std::string, Symbol *, std::less<>> NameSymbolMap;

struct SCCNode;
class CallGraph;
// Contains information about a specific symbol.
// There are two types of symbols:
// 1. Actual symbol: the symbol exists in the binary as a standalone function.
//                   It has the begin_address and end_address, and its name
//                   is always full assembler name.
// 2. Inlined symbol: the symbol is cloned in another function. It does not
//                    have the begin_address and end_address, and its name
//                    could be a short bfd_name.
class Symbol {
 public:
  // This constructor is used to create inlined symbol.
#if defined(HAVE_LLVM)
  Symbol(const char *name, llvm::StringRef dir, llvm::StringRef file,
         uint32_t start)
#else
  Symbol(const char *name, std::string dir, std::string file, uint32_t start)
#endif
      : info(SourceInfo(name, dir, file, start, 0, 0)),
        total_count(0),
        total_count_incl(0),
        head_count(0),
        callsites(0),
        pos_counts() {
  }

  // This constructor is used to create aliased symbol.
  Symbol(const Symbol *src, const char *new_func_name)
      : info(src->info),
        total_count(src->total_count),
        total_count_incl(src->total_count_incl),
        head_count(src->head_count),
        callsites(0),
        pos_counts() {
    info.func_name = new_func_name;
  }

  Symbol()
      : total_count(0),
        total_count_incl(0),
        head_count(0),
        callsites(0),
        pos_counts() {}

  ~Symbol();

  static std::string Name(const char *name) {
    return (name && strlen(name) > 0) ? name : "noname";
  }

  std::string name() const { return Name(info.func_name); }

  void PopulateSymbolRetainingHotInlineStacks(Symbol &, const uint64_t,
                                              SymbolMap &, uint64_t &,
                                              uint64_t &);

  // Merges profile stored in src symbol with this symbol.
  void Merge(const Symbol *src);

  // Get an estimation of head count from the starting source or callsite
  // locations.
  void EstimateHeadCount();

  // Convert an inline instance profile into a callsite location.
  void FlattenCallsite(uint64_t offset, const Symbol *callee);

  // Merges flat profile stored in src symbol with this symbol.
  void FlatMerge(const Symbol *src);

  // Update each count with count * ratio inside current symbol.
  void UpdateWithRatio(double ratio);

  void ComputeTotalCountIncl(const NameSymbolMap &nsmap,
                             std::vector<Symbol *> *stacksyms,
                             absl::flat_hash_set<Symbol *> *scc);
  void BuildCallGraph(const NameSymbolMap &nsmap, SCCNode *callerSCC,
                      CallGraph *callgraph);

  // Dumps the body of the symbol.
  void DumpBody(int indent, bool for_analysis) const;
  // Dumps content of the symbol with a give indentation.
  void Dump(int indent) const;
  // Similar as Dump, but with information for performance analysis.
  void DumpForAnalysis(int indent) const;

  // Limits the number of inline instances at the same callsite location to the
  // specified value. This is applied recursively if the symbol has nested
  // inlined functions. Returns the total count of erased inlined functions.
  int64_t throttleInlineInstancesAtSameLocation(int max_inline_instances);

  // Limits call targets to specified number for each ProfileInfo in pos_counts,
  // keeping those with the most count.
  void TrimCallTargets(int64_t max_call_targets);

  // Returns the entry count based on pos_counts and callsites.
  uint64_t EntryCount() const;

  // Source information about the symbol (func_name, file_name, etc.)
  SourceInfo info;
  // The total sampled count, including all the samples collected from
  // current symbol, but not including those collected from any callee
  // of the symbol.
  uint64_t total_count;
  // The total sampled count, including all the sample counts from
  // current symbol and all its decedents called by the symbol.
  uint64_t total_count_incl;
  // The total sampled count in the head bb.
  uint64_t head_count;
  // Map from callsite location to callee symbol.
  CallsiteMap callsites;
  // Map from source location to count and instruction number.
  PositionCountMap pos_counts;
};

// Vector of unique pointers to symbols.
typedef std::vector<std::unique_ptr<Symbol>> SymbolUniquePtrVector;
// Maps symbol's start address to its name and size.
typedef std::map<uint64_t, std::pair<std::string, uint64_t>> AddressSymbolMap;
// Maps from symbol's name to its start address.
typedef std::map<std::string, uint64_t> NameAddressMap;
// Maps function name to alias names.
typedef absl::node_hash_map<std::string, absl::flat_hash_set<std::string>>
    NameAliasMap;
#if defined(HAVE_LLVM)
// List of pairs containing function name and size.
using NameSizeList = std::vector<std::pair<llvm::StringRef, uint64_t>>;
#endif

// SymbolMap stores the symbols in the binary, and maintains
// a map from symbol name to its related information.
class SymbolMap {
 public:
  explicit SymbolMap(absl::string_view binary)
      : binary_(binary),
        count_threshold_(0),
        ignore_thresholds_(false),
        suffix_elision_policy_(ElideAll) {
    initSuffixElisionPolicy();
    if (!binary.empty()) {
      BuildSymbolMap();
      BuildNameAddressMap();
    }
  }

  SymbolMap() : count_threshold_(0), suffix_elision_policy_(ElideAll) {
    initSuffixElisionPolicy();
  }

  // This type is neither copyable nor movable.
  SymbolMap(const SymbolMap &) = delete;
  SymbolMap &operator=(const SymbolMap &) = delete;

  static bool IsLLVMCompiler(absl::string_view path);

  // Return the fs_discriminator flag variable name.
  static const char *get_fs_discriminator_symbol() {
    return "__llvm_fs_discriminator__";
  }

  uint64_t size() const { return map_.size(); }

  void set_count_threshold(int64_t n) { count_threshold_ = n; }
  int64_t count_threshold() const { return count_threshold_; }

  void set_suffix_elision_policy(const std::string &policy);
  const std::string suffix_elision_policy() const;

  // Returns true if the count is large enough to be emitted.
  bool ShouldEmit(int64_t count) const { return count > count_threshold_; }

  // Calculates sample threshold from given total count.
  void CalculateThresholdFromTotalCount(int64_t total_count);

  // Calculates sample threshold from symbol map.
  // All symbols should have been counted.
  void CalculateThreshold();

  // Read loadable exec segment information from binary.
  // If is_kernel set to true, this is for linux kernel (this information
  // is from perf file (i.e. from sample_reader).
  void ReadLoadableExecSegmentInfo(bool is_kernel);

  // Returns static virtual address for offset.
  //
  // The formula to translate the runtime vaddr to the static vaddr is:
  // "vaddr = curr_exec_segment.vaddr +
  //          (instr.file_offset - curr_exec_segment.file_offset)"
  // Here the instr.file_offset is computed and save as offset in parsed event:
  // "instr.file_offset = runtime_vaddr - mmap_event.start + mmap_event.pgoff".
  //
  // This function returns the vaddress for a given offset (file-offset in the
  // DSO or the binary file):
  // "vaddr = exec_segment.vaddr - exec_segment.file_offset + offset".
  //
  uint64_t get_static_vaddr(uint64_t offset) const {
    uint64_t unset_addr = std::numeric_limits<uint64_t>::max();
    uint64_t base_addr = unset_addr;
    for (auto &seg : loadable_exec_segments_) {
      if (offset < seg.offset) break;
      base_addr = seg.vaddr - seg.offset;
    }
    assert(base_addr != unset_addr);
    return offset + base_addr;
  }

  // Converts a static virtual address into an offset.
  //
  // The inverse of `get_static_vaddr`.
  uint64_t GetFileOffsetFromStaticVaddr(uint64_t static_vaddr) const {
    auto it = absl::c_find_if(
        loadable_exec_segments_,
        [&](const segmentinfo &seg) { return seg.vaddr > static_vaddr; });
    CHECK(it != loadable_exec_segments_.begin())
        << "No loadable exec segment found for static vaddr: " << static_vaddr;
    --it;
    return it->offset + static_vaddr - it->vaddr;
  }

  void set_ignore_thresholds(bool v) {
    ignore_thresholds_ = v;
  }

  void set_addr2line(std::unique_ptr<Addr2line> addr2line) {
    addr2line_ = std::move(addr2line);
  }

  Addr2line *get_addr2line() const { return addr2line_.get(); }

  // Adds an empty named symbol.
  void AddSymbol(absl::string_view name);

  // Removes a symbol by setting total and head count to zero.
  void RemoveSymbol(absl::string_view name);

  // Removes all the out of line symbols matching the regular expression
  // "regex_str" by setting their total and head counts to zero. Those
  // symbols with zero counts will be removed when profile is written out.
  void RemoveSymsMatchingRegex(absl::string_view regex_str);

  // Adds the given symbols and their mappings to the symbol map. SymbolMap
  // takes ownership of the symbols in new_map. Existing mappings in SymbolMap
  // that overlap with entries in new_map, will be updated to the new symbols.
  void AddSymbolMappings(const NameSymbolMap &new_map);

  const NameSymbolMap &map() const {
    return map_;
  }

  const NameAddressMap &GetNameAddrMap() const { return name_addr_map_; }

  const gcov_working_set_info *GetWorkingSets() const {
    return working_set_;
  }

  void UpdateWorkingSet(int i, uint32_t num_counters, uint64_t min_counter) {
    if (working_set_[i].num_counters == 0) {
      working_set_[i].num_counters = num_counters;
    } else {
      // This path only happens during profile merge.
      // Different profiles will have similar num_counters, so calculating
      // average for each iteration will no lose much precision.
      working_set_[i].num_counters =
          (working_set_[i].num_counters + num_counters) / 2;
    }
    working_set_[i].min_counter += min_counter;
  }

  const Symbol *GetSymbolByName(absl::string_view name) const {
    NameSymbolMap::const_iterator ret = map_.find(name);
    if (ret != map_.end()) {
      return ret->second;
    } else {
      return nullptr;
    }
  }

  // Trims suffix from name, returning trimmed name (according to
  // current suffix elision policy).
  std::string GetOriginalName(absl::string_view name) const;

  // Merges symbols with suffixes like .isra, .part, or .llvm as a single
  // symbol, and elides the suffixes. These suffixes are not stable between
  // compilations, and the compiler is also expected to elide them when matching
  // profile data.
  void ElideSuffixesAndMerge();

  // Increments symbol's entry count.
  void AddSymbolEntryCount(absl::string_view symbol, uint64_t head_count,
                           uint64_t total_count = 0);

  // DataSource represents what kind of data is used to generate afdo profile.
  // PERFDATA: convert perf.data to afdo profile.
  // AFDOPROTO: convert afdo proto generated by GWP autofdo pipeline to
  //            afdo profile.
  // AFDOPROFILE: merge an existing afdo profile into current one.
  typedef enum { INVALID = 0, PERFDATA, AFDOPROTO, AFDOPROFILE } DataSource;
  // Increments source stack's count.
  //   symbol: name of the symbol in which source is located.
  //   source: source location (in terms of inlined source stack).
  //   count: total sampled count.
  //   num_inst: number of instructions that is mapped to the source.
  //   duplication: multiply count with this value.
  //   data_source: the type of data used to generate autofdo profile.
  //   Typically it is perf data, autofdo proto or some other autofdo
  //   profile.
  void AddSourceCount(absl::string_view symbol, const SourceStack &source,
                      uint64_t count, uint64_t num_inst,
                      uint32_t duplication = 1,
                      DataSource data_source = AFDOPROFILE);

  // Generates hybrid profiles by flattening callsites whose total counts are
  // below the threshold, recursively. This is a fine-grained flattening
  // algorithm that allows inline calls close to the top-level function to
  // persist while their colder callees are flattened. This provides a good
  // balance between profile size and performance.
  void BuildHybridProfile(const SymbolMap &srcmap, uint64_t threshold,
                          uint64_t &num_callsites, uint64_t &num_flattened);

  // Selectively convert hierarchical profiles into flat profiles.
  // Hierarchical profiles contains context information for optimization. Flat
  // profiles contain do not contain context information. This tradeoff allows
  // for smaller profile size (leading to smaller XFDO profiles) and lower
  // Forge/Piper costs. Selective flattening allows hot functions to retain
  // context sensitive information, while removing it from cold functions to
  // strike a balance between optimization and Forge/Piper costs.
  void BuildFlatProfile(const SymbolMap & srcmap, bool selectively_flatten,
                        uint64_t threshold, uint64_t &num_total_functions,
                        uint64_t &num_flattened);

  // Limits inline callsite nesting level to
  // `max_inline_callsite_nesting_level`, and flattens further inlined
  // callsites. Returns number of function flattened.
  int FlattenNestedInlineCallsites(int max_inline_callsite_nesting_level);

  void AddSymbolToMap(const Symbol & symbol);

  // Update each count inside of the map with count * ratio.
  void UpdateWithRatio(double ratio);

  // Return the sum of total counts of all the outline symbols
  uint64_t GetTotalSum();

  // Adds the indirect call target to source stack.
  //   symbol: name of the symbol in which source is located.
  //   source: source location (in terms of inlined source stack).
  //   target: indirect call target.
  //   count: total sampled count.
  //   data_source: the type of data used to generate autofdo profile.
  //   Typically it is perf data, autofdo proto or some other autofdo
  //   profile.
  // Returns false if we failed to add the call target.
  bool AddIndirectCallTarget(absl::string_view symbol, const SourceStack &src,
                             absl::string_view target, uint64_t count,
                             DataSource data_source = AFDOPROFILE);

  // Traverses the inline stack in source, update the symbol map by adding
  // count to the total count in the inlined symbol. Returns the leaf symbol. If
  // the inline stack is empty, returns nullptr without any other updates.
  //   data_source: the type of data used to generate autofdo profile.
  //   Typically it is perf data, autofdo proto or some other autofdo
  //   profile.
  Symbol *TraverseInlineStack(absl::string_view symbol,
                              const SourceStack &source, uint64_t count,
                              DataSource data_source = AFDOPROFILE);

  // Updates function name, start_addr, end_addr of a function that has a
  // given address. Returns false if no such symbol exists.
  const bool GetSymbolInfoByAddr(uint64_t addr, const std::string **name,
                                 uint64_t *start_addr,
                                 uint64_t *end_addr) const;

  // Returns a pointer to the symbol name for a given start address. Returns
  // nullptr if no such symbol exists.
  const std::string *GetSymbolNameByStartAddr(uint64_t address) const;

  // Returns the overlap between two symbol maps. For two profiles, if
  // count_i_j denotes the function count of the ith function in profile j;
  // total_j denotes the total count of all functions in profile j. Then
  // overlap = sum(min(count_i_1/total_1, count_i_2/total_2))
  float Overlap(const SymbolMap &map) const;

  // Iterates the address count map to calculate the working set of the profile.
  // Working set is a map from bucket_num to total number of instructions that
  // consumes bucket_num/NUM_GCOV_WORKING_SETS of dynamic instructions. This
  // mapping indicates how large is the dynamic hot code region during run time.
  //
  // To compute working set, the following algorithm is used:
  //
  // Input: map from instruction to execution count.
  // Output: working set.
  //   1. compute histogram: map (execution count --> number of instructions)
  //   2. traverse the histogram in descending order
  //     2.1 calculate accumulated_count.
  //     2.2 compute the working set bucket number.
  //     2.3 update the working set bucket from last update to calculated bucket
  //         number.
  void ComputeWorkingSets();

  // Returns a map from start addresses of functions that have been sampled to
  // the size of the function.
  std::map<uint64_t, uint64_t> GetSampledSymbolStartAddressSizeMap(
      const std::set<uint64_t> &sampled_addrs) const;

  void ComputeTotalCountIncl();
  void BuildCallGraph(CallGraph *callgraph);

  void Dump(bool dump_for_analysis = false) const;
  void DumpFuncLevelProfileCompare(const SymbolMap &map) const;

  void AddAlias(absl::string_view sym, const std::string &alias);

  // Validates if the current symbol map is sane.
  bool Validate() const;

  // For scenarios such as llc misses or profile-guided prefetching, we need to
  // setup the symbol map such that the func_name appears to have samples.
  // That data is ignored by readers.
  bool EnsureEntryInFuncForSymbol(absl::string_view func_name, uint64_t pc);

#if defined(HAVE_LLVM)
  // Collect all function symbols and size in address_symbol_map_, but remove
  // the names showing up in the profile.
  NameSizeList collectNamesForProfSymList();

  // Collect all names including names of outline instances, inline instances
  // and call targets in current map.
  llvm::StringSet<> collectNamesInProfile();
#endif

  // Limits the number of inline instances at the same callsite location in this
  // sample map, keeping those with the most count.
  // An example is multiple call targets can be inlined for the same indirect
  // call. If there are too many inline instances in the profile for the same
  // location, that can introduce excessive cost in ThinLTO importing without
  // apparent performance benefit, and we better use some throttling.
  void throttleInlineInstancesAtSameLocation(int max_inline_instances);

  // Limits call targets to specified number for all profiled locations in this
  // sample map, keeping those with the most count.
  void TrimCallTargets(int64_t max_call_targets);

 private:
  // Reads from the binary's elf section to build the symbol map.
  void BuildSymbolMap();

  // Initialize suffix elision policy from flags.
  void initSuffixElisionPolicy();

  // Reads from address_symbol_map_ and update name_addr_map_.
  void BuildNameAddressMap() {
    for (const auto &[addr, symbol] : address_symbol_map_) {
      name_addr_map_[symbol.first] = addr;
    }
  }

  void add_loadable_exec_segment(uint64_t offset, uint64_t vaddr) {
    // Check the offset field in loadable_exec_segments is in ascending order.
    assert(loadable_exec_segments_.empty() ||
           loadable_exec_segments_.back().offset < offset);
    loadable_exec_segments_.push_back({offset, vaddr});
  }

  // Implementation of FlattenNestedInlineCallsites(). Recursively traverses
  // nested inline callsites and flatten them if their nesting level reach the
  // maximum. Returns difference of `func`'s total count after flattening.
  int64_t FlattenNestedInlineCallsitesImpl(
      int max_inline_callsite_nesting_level, int depth, Symbol *func);

  // Store the loadable_exec_segment info:
  // offset is the offset of the segment, and
  // vaddr is the vaddr of the segment.
  typedef struct {
    uint64_t offset;
    uint64_t vaddr;
  } segmentinfo;

  SymbolUniquePtrVector unique_symbols_;  // Owns the symbols.
  NameSymbolMap map_;
  NameAliasMap name_alias_map_;
  NameAddressMap name_addr_map_;
  AddressSymbolMap address_symbol_map_;
  const std::string binary_;
  // segments needs to sort by offset in ascending order.
  std::vector<segmentinfo> loadable_exec_segments_;
  int64_t count_threshold_;
  bool ignore_thresholds_;
  uint8_t suffix_elision_policy_;
  std::unique_ptr<Addr2line> addr2line_;
  /* working_set_[i] stores # of instructions that consumes
     i/NUM_GCOV_WORKING_SETS of total instruction counts.  */
  gcov_working_set_info working_set_[NUM_GCOV_WORKING_SETS];

  enum {
    ElideAll = 0,
    ElideSelected = 1,
    ElideNone = 2
  };
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_SYMBOL_MAP_H_
