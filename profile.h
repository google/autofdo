// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to extract function level profile from binary level samples.

#ifndef AUTOFDO_PROFILE_H_
#define AUTOFDO_PROFILE_H_

#include <cstdint>
#include <set>
#include <string>

#include "base/integral_types.h"
#include "sample_reader.h"
#include "third_party/abseil/absl/container/node_hash_map.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {

class Addr2line;
class SymbolMap;

// Class to convert instruction level profile to source level profile.
class Profile {
 public:
  // Arguments:
  //   sample_reader: the sample reader provides the instruction level profile,
  //                  i.e. map from instruction/callstack to profile counts.
  //   binary_name: the binary file name.
  //   addr2line: an Addr2line.
  //   symbol_map: the symbol map is written by this class to store all symbol
  //               information.
  Profile(const SampleReader *sample_reader, absl::string_view binary_name,
          Addr2line *addr2line, SymbolMap *symbol_map)
      : sample_reader_(sample_reader),
        binary_name_(binary_name),
        addr2line_(addr2line),
        symbol_map_(symbol_map) {}

  // This type is neither copyable nor movable.
  Profile(const Profile &) = delete;
  Profile &operator=(const Profile &) = delete;

  ~Profile();

  // Builds the source level profile. When check_lbr_entry is true,
  // use MiniDisassembler to check if the from address of range_map
  // is a branch, call, or return instruction.
  void ComputeProfile(bool check_lbr_entry = false);

 private:
  // Internal data structure that aggregates profile for each symbol.
  struct ProfileMaps {
    ProfileMaps(uint64_t start, uint64_t end)
        : start_addr(start), end_addr(end) {}
    uint64_t GetAggregatedCount() const;
    uint64_t start_addr;
    uint64_t end_addr;
    AddressCountMap address_count_map;
    RangeCountMap range_count_map;
    BranchCountMap branch_count_map;
  };
  typedef absl::node_hash_map<std::string, ProfileMaps *> SymbolProfileMaps;

  // Returns the profile maps for a give function.
  ProfileMaps *GetProfileMaps(uint64_t addr);

  // Aggregates raw profile for each symbol.
  void AggregatePerFunctionProfile(bool check_lbr_entry);

  // Builds function level profile for specified function:
  //   1. Traverses all instructions to build instruction map.
  //   2. Unwinds the inline stack to add symbol count to each inlined symbol.
  void ProcessPerFunctionProfile(const std::string &func_name,
                                 const ProfileMaps &map);

  const SampleReader *sample_reader_;
  const std::string binary_name_;
  Addr2line *addr2line_;
  SymbolMap *symbol_map_;
  AddressCountMap global_addr_count_map_;
  SymbolProfileMaps symbol_profile_maps_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_PROFILE_H_
