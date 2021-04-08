// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to represent source level profile.
#include "profile.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "instruction_map.h"
#include "sample_reader.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/match.h"
#include "third_party/abseil/absl/strings/strip.h"

ABSL_FLAG(bool, use_lbr, true,
            "Whether to use lbr profile.");
ABSL_FLAG(bool, llc_misses, false, "The profile represents llc misses.");

namespace devtools_crosstool_autofdo {
Profile::ProfileMaps *Profile::GetProfileMaps(uint64 addr) {
  const std::string *name;
  uint64 start_addr, end_addr;
  if (symbol_map_->GetSymbolInfoByAddr(addr, &name,
                                       &start_addr, &end_addr)) {
    std::pair<SymbolProfileMaps::iterator, bool> ret =
        symbol_profile_maps_.insert(
            SymbolProfileMaps::value_type(*name, nullptr));
    if (ret.second) {
      ret.first->second = new ProfileMaps(start_addr, end_addr);
    }
    return ret.first->second;
  } else {
    return nullptr;
  }
}

void Profile::AggregatePerFunctionProfile() {
  uint64 start = symbol_map_->base_addr();
  const AddressCountMap *count_map = &sample_reader_->address_count_map();
  for (const auto &addr_count : *count_map) {
    ProfileMaps *maps = GetProfileMaps(addr_count.first + start);
    if (maps != nullptr) {
      maps->address_count_map[addr_count.first + start] += addr_count.second;
    }
  }
  const RangeCountMap *range_map = &sample_reader_->range_count_map();
  for (const auto &range_count : *range_map) {
    ProfileMaps *maps = GetProfileMaps(range_count.first.first + start);
    if (maps != nullptr) {
      maps->range_count_map[std::make_pair(range_count.first.first + start,
                                           range_count.first.second + start)] +=
          range_count.second;
    }
  }
  const BranchCountMap *branch_map = &sample_reader_->branch_count_map();
  for (const auto &branch_count : *branch_map) {
    ProfileMaps *maps = GetProfileMaps(branch_count.first.first + start);
    if (maps != nullptr) {
      maps->branch_count_map[std::make_pair(
          branch_count.first.first + start,
          branch_count.first.second + start)] += branch_count.second;
    }
  }

  // Add an entry for each symbol so that later we can decide if the hot and
  // cold parts together need to be emitted.
  for (const auto &[name, addr] : symbol_map_->GetNameAddrMap()) {
    CHECK(GetProfileMaps(addr));
  }
}

uint64 Profile::ProfileMaps::GetAggregatedCount() const {
  uint64 ret = 0;

  if (!range_count_map.empty()) {
    for (const auto &range_count : range_count_map) {
      ret += range_count.second * (1 + range_count.first.second -
                                   range_count.first.first);
    }
  } else {
    for (const auto &addr_count : address_count_map) {
      ret += addr_count.second;
    }
  }
  return ret;
}

void Profile::ProcessPerFunctionProfile(std::string func_name,
                                        const ProfileMaps &maps) {
  InstructionMap inst_map(addr2line_, symbol_map_);
  inst_map.BuildPerFunctionInstructionMap(func_name, maps.start_addr,
                                          maps.end_addr);

  AddressCountMap map;
  const AddressCountMap *map_ptr;
  if (absl::GetFlag(FLAGS_use_lbr)) {
    if (maps.range_count_map.empty()) {
      LOG(WARNING) << "use_lbr was enabled but range_count_map was empty!";
      return;
    }
    for (const auto &range_count : maps.range_count_map) {
      for (InstructionMap::InstMap::const_iterator iter =
               inst_map.inst_map().find(range_count.first.first);
           iter != inst_map.inst_map().end()
               && iter->first <= range_count.first.second;
           ++iter) {
        map[iter->first] += range_count.second;
      }
    }
    map_ptr = &map;
  } else {
    map_ptr = &maps.address_count_map;
  }

  for (const auto &address_count : *map_ptr) {
    InstructionMap::InstMap::const_iterator iter =
        inst_map.inst_map().find(address_count.first);
    if (iter == inst_map.inst_map().end()) {
      continue;
    }
    const InstructionMap::InstInfo *info = iter->second;
    if (info == nullptr) {
      continue;
    }
    if (!info->source_stack.empty()) {
      symbol_map_->AddSourceCount(
          func_name, info->source_stack,
          address_count.second * info->source_stack[0].DuplicationFactor(), 0,
          SymbolMap::PERFDATA);
    }
  }

  for (const auto &branch_count : maps.branch_count_map) {
    InstructionMap::InstMap::const_iterator iter =
        inst_map.inst_map().find(branch_count.first.first);
    if (iter == inst_map.inst_map().end()) {
      continue;
    }
    const InstructionMap::InstInfo *info = iter->second;
    if (info == nullptr) {
      continue;
    }
    const std::string *callee =
        symbol_map_->GetSymbolNameByStartAddr(branch_count.first.second);
    if (!callee) {
      continue;
    }
    if (symbol_map_->map().count(*callee)) {
      symbol_map_->AddSymbolEntryCount(*callee, branch_count.second);
      symbol_map_->AddIndirectCallTarget(func_name, info->source_stack, *callee,
                                         branch_count.second,
                                         SymbolMap::PERFDATA);
    }
  }

  for (const auto &addr_count : *map_ptr) {
    global_addr_count_map_[addr_count.first] = addr_count.second;
  }
}

void Profile::ComputeProfile() {
  symbol_map_->CalculateThresholdFromTotalCount(
      sample_reader_->GetTotalCount());
  AggregatePerFunctionProfile();

  if (absl::GetFlag(FLAGS_llc_misses)) {
    for (const auto &symbol_profile : symbol_profile_maps_) {
      const auto func_name = symbol_profile.first;
      const auto &maps = *symbol_profile.second;

      std::map<uint64_t, uint64_t> counts;
      for (const auto &address_count : maps.address_count_map) {
        auto pc = address_count.first;
        DCHECK(maps.start_addr <= pc && pc <= maps.end_addr);
        if (!symbol_map_->EnsureEntryInFuncForSymbol(func_name, pc))
          continue;
        counts[pc] += address_count.second;
      }

      CHECK(maps.branch_count_map.empty());
      for (const auto pair : counts) {
        uint64_t pc = pair.first;
        uint64_t count = pair.second;
        SourceStack stack;
        symbol_map_->get_addr2line()->GetInlineStack(pc, &stack);
        symbol_map_->AddIndirectCallTarget(func_name, stack, "__llc_misses__",
                                           count);
      }
    }
    symbol_map_->ElideSuffixesAndMerge();
  } else {
    // Precompute the aggregated counts of hot and cold parts. Both function
    // parts are emitted only if their total sample count is above the required
    // threshold.
    absl::flat_hash_map<absl::string_view, uint64> symbol_counts;
    for (const auto &[name, profile] : symbol_profile_maps_) {
      symbol_counts[absl::StripSuffix(name, ".cold")] +=
          profile->GetAggregatedCount();
    }

    // First add all symbols that needs to be outputted to the symbol_map_. We
    // need to do this before hand because ProcessPerFunctionProfile will call
    // AddSymbolEntryCount for other symbols, which may or may not had been
    // processed by ProcessPerFunctionProfile.
    for (const auto &[name, ignored] : symbol_profile_maps_) {
      const uint64 count = symbol_counts.at(absl::StripSuffix(name, ".cold"));
      if (symbol_map_->ShouldEmit(count)) {
        symbol_map_->AddSymbol(name);
      }
    }

    for (const auto &[name, profile] : symbol_profile_maps_) {
      const uint64 count = symbol_counts.at(absl::StripSuffix(name, ".cold"));
      if (symbol_map_->ShouldEmit(count)) {
        ProcessPerFunctionProfile(name, *profile);
      }
    }
    symbol_map_->ElideSuffixesAndMerge();
    symbol_map_->ComputeWorkingSets();
  }
}

Profile::~Profile() {
  for (auto &symbol_maps : symbol_profile_maps_) {
    delete symbol_maps.second;
  }
}
}  // namespace devtools_crosstool_autofdo
