// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Class to represent the symbol map. The symbol map is a map from
// symbol names to the symbol class.
// This class is thread-safe.

#ifndef AUTOFDO_SYMBOL_MAP_H_
#define AUTOFDO_SYMBOL_MAP_H_

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/common.h"
#include "base/macros.h"
#include "source_info.h"

// Macros from gcc (profile.c)
#define NUM_GCOV_WORKING_SETS 128
#define WORKING_SET_INSN_PER_BB 10

namespace autofdo {

typedef map<string, uint64> CallTargetCountMap;
typedef pair<string, uint64> TargetCountPair;
typedef vector<TargetCountPair> TargetCountPairs;

class Addr2line;

/* Struct from gcc (basic-block.h).
   Working set size statistics for a given percentage of the entire
   profile (sum_all from the counter summary).  */
struct gcov_working_set_info {
 public:
  gcov_working_set_info() : num_counters(0), min_counter(0) {}
  /* Number of hot counters included in this working set.  */
  uint32 num_counters;
  /* Smallest counter included in this working set.  */
  uint64 min_counter;
};

// Returns a sorted vector of target_count pairs. target_counts is a pointer
// to an empty vector in which the output will be stored.
// Sorting is based on count in descending order.
void GetSortedTargetCountPairs(const CallTargetCountMap &call_target_count_map,
                               TargetCountPairs *target_counts);

// Represents profile information of a given source.
class ProfileInfo {
 public:
  ProfileInfo() : count(0), num_inst(0) {}
  ProfileInfo &operator+=(const ProfileInfo &other);

  uint64 count;
  uint64 num_inst;
  CallTargetCountMap target_map;
};

// Map from source stack to profile,
// TODO(dehao): deprecate this when old profile format is deprecated.
typedef map<const SourceStack, ProfileInfo> SourceStackCountMap;

// Map from a source location (represented by offset+discriminator) to profile.
typedef map<uint32, ProfileInfo> PositionCountMap;

// callsite_location, callee_name
typedef pair<uint32, const char *> Callsite;

struct CallsiteLess {
  bool operator()(const Callsite& c1, const Callsite& c2) const {
    if (c1.first != c2.first)
      return c1.first < c2.first;
    if ((c1.second == NULL || c2.second == NULL))
      return c1.second < c2.second;
    return strcmp(c1.second, c2.second) < 0;
  }
};
class Symbol;
// Map from a callsite to the callee symbol.
typedef map<Callsite, Symbol *, CallsiteLess> CallsiteMap;

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
  Symbol(const char *name, const char *dir, const char *file, uint32 start)
      : info(SourceInfo(name, dir, file, start, 0, 0)),
        total_count(0), head_count(0) {}

  Symbol() : total_count(0), head_count(0) {}

  ~Symbol();

  // Merges profile stored in src symbol with this symbol.
  void Merge(const Symbol *src);

  // Returns the module name of the symbol. Module name is the source file
  // that the symbol belongs to. It is an attribute of the actual symbol.
  string ModuleName() const;

  // Returns true if the symbol is from a header file.
  bool IsFromHeader() const;

  // Dumps content of the symbol with a give indentation.
  void Dump(int indent) const;

  // Source information about the the symbol (func_name, file_name, etc.)
  SourceInfo info;
  // The total sampled count.
  uint64 total_count;
  // The total sampled count in the head bb.
  uint64 head_count;
  // Map from callsite location to callee symbol.
  CallsiteMap callsites;
  // Map from source location to count and instruction number.
  PositionCountMap pos_counts;
};

// Maps function name to actual symbol. (Top level map).
typedef map<string, Symbol *> NameSymbolMap;
// Maps symbol's start address to its name and size.
typedef map<uint64, pair<string, uint64> > AddressSymbolMap;
// Maps from symbol's name to its start address.
typedef map<string, uint64> NameAddressMap;
// Maps function name to alias names.
typedef map<string, set<string> > NameAliasMap;

// SymbolMap stores the symbols in the binary, and maintains
// a map from symbol name to its related information.
class SymbolMap {
 public:
  explicit SymbolMap(const string &binary)
      : binary_(binary), base_addr_(0) {
    BuildSymbolMap();
    BuildNameAddressMap();
  }

  explicit SymbolMap() {}

  ~SymbolMap();

  uint64 size() const {
    return map_.size();
  }

  void set_count_threshold(int64 n) {count_threshold_ = n;}
  int64 count_threshold() const {return count_threshold_;}

  // Returns true if the count is large enough to be emitted.
  bool ShouldEmit(int64 count) const {
    CHECK_GT(count_threshold_, 0);
    return count > count_threshold_;
  }

  // Caculates sample threshold from given total count.
  void CalculateThresholdFromTotalCount(int64 total_count);

  // Caculates sample threshold from symbol map.
  // All symbols should have been counted.
  void CalculateThreshold();

  // Returns relocation start address.
  uint64 base_addr() const {
    return base_addr_;
  }

  // Adds an empty named symbol.
  void AddSymbol(const string &name);

  const NameSymbolMap &map() const {
    return map_;
  }

  const gcov_working_set_info *GetWorkingSets() const {
    return working_set_;
  }

  uint64 GetSymbolStartAddr(const string &name) const {
    const auto &iter = name_addr_map_.find(name);
    if (iter == name_addr_map_.end()) {
      return 0;
    }
    return iter->second;
  }

  void UpdateWorkingSet(int i, uint32 num_counters, uint64 min_counter) {
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

  const Symbol *GetSymbolByName(const string &name) const {
    NameSymbolMap::const_iterator ret = map_.find(name);
    if (ret != map_.end()) {
      return ret->second;
    } else {
      return NULL;
    }
  }

  // Merges symbols with suffixes like .isra, .part as a single symbol.
  void Merge();

  // Increments symbol's entry count.
  void AddSymbolEntryCount(const string &symbol, uint64 count);

  typedef enum {INVALID = 1, SUM, MAX} Operation;
  // Increments source stack's count.
  //   symbol: name of the symbol in which source is located.
  //   source: source location (in terms of inlined source stack).
  //   count: total sampled count.
  //   num_inst: number of instructions that is mapped to the source.
  //   op: operation used to calculate count (SUM or MAX).
  void AddSourceCount(const string &symbol, const SourceStack &source,
                      uint64 count, uint64 num_inst, Operation op);

  // Adds the indirect call target to source stack.
  //   symbol: name of the symbol in which source is located.
  //   source: source location (in terms of inlined source stack).
  //   target: indirect call target.
  //   count: total sampled count.
  void AddIndirectCallTarget(const string &symbol, const SourceStack &source,
                             const string &target, uint64 count);

  // Traverses the inline stack in source, update the symbol map by adding
  // count to the total count in the inlined symbol. Returns the leaf symbol.
  Symbol *TraverseInlineStack(const string &symbol, const SourceStack &source,
                              uint64 count);

  // Updates function name, start_addr, end_addr of a function that has a
  // given address. Returns false if no such symbol exists.
  const bool GetSymbolInfoByAddr(uint64 addr, const string **name,
                                 uint64 *start_addr, uint64 *end_addr) const;

  // Returns a pointer to the symbol name for a given start address. Returns
  // NULL if no such symbol exists.
  const string *GetSymbolNameByStartAddr(uint64 address) const;

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
  //   2. traverse the histogram in decending order
  //     2.1 calculate accumulated_count.
  //     2.2 compute the working set bucket number.
  //     2.3 update the working set bucket from last update to calculated bucket
  //         number.
  void ComputeWorkingSets();

  // Updates the symbol from new binary.
  //   * reads the module info stored by "-frecord-compilation-info-in-elf".
  //   * updates each symbol's module info from the debug info stored in
  //     addr2line.
  //   * re-groups the module from the updated module info.
  void UpdateSymbolMap(const string &binary, const Addr2line *addr2line);

  // Returns a map from start addresses of functions that have been sampled to
  // the size of the function.
  ::map<uint64, uint64> GetSampledSymbolStartAddressSizeMap(
      const set<uint64> &sampled_addrs) const;

  void Dump() const;
  void DumpFuncLevelProfileCompare(const SymbolMap &map) const;

  void AddAlias(const string& sym, const string& alias);

  // Validates if the current symbol map is sane.
  bool Validate() const;

 private:
  // Reads from the binary's elf section to build the symbol map.
  void BuildSymbolMap();

  // Reads from address_symbol_map_ and update name_addr_map_.
  void BuildNameAddressMap() {
    for (const auto &addr_symbol : address_symbol_map_) {
      name_addr_map_[addr_symbol.second.first] = addr_symbol.first;
    }
  }

  NameSymbolMap map_;
  NameAliasMap name_alias_map_;
  NameAddressMap name_addr_map_;
  AddressSymbolMap address_symbol_map_;
  const string binary_;
  uint64 base_addr_;
  int64 count_threshold_;
  /* working_set_[i] stores # of instructions that consumes
     i/NUM_GCOV_WORKING_SETS of total instruction counts.  */
  gcov_working_set_info working_set_[NUM_GCOV_WORKING_SETS];

  DISALLOW_COPY_AND_ASSIGN(SymbolMap);
};
}  // namespace autofdo

#endif  // AUTOFDO_SYMBOL_MAP_H_
