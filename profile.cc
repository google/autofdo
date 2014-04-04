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

// Class to represent source level profile.

#include "profile.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/common.h"
#include "instruction_map.h"
#include "symbol_map.h"
#include "sample_reader.h"

DEFINE_bool(use_lbr, true,
            "Whether to use lbr profile.");
DEFINE_uint64(sample_threshold, 50000,
              "Sample threshold divider. The threshold of total function count"
              " is determined by max_sample_count/sample_threshold.");

namespace autofdo {
Profile::ProfileMaps *Profile::GetProfileMaps(uint64 addr) {
  const string *name;
  uint64 start_addr, end_addr;
  if (symbol_map_->GetSymbolInfoByAddr(addr, &name,
                                       &start_addr, &end_addr)) {
    pair<SymbolProfileMaps::iterator, bool> ret = symbol_profile_maps_.insert(
        SymbolProfileMaps::value_type(*name, NULL));
    if (ret.second) {
      ret.first->second = new ProfileMaps(start_addr, end_addr);
    }
    return ret.first->second;
  } else {
    return NULL;
  }
}

void Profile::AggregatePerFunctionProfile() {
  uint64 start = symbol_map_->base_addr();
  const AddressCountMap *count_map = &sample_reader_->address_count_map();
  for (const auto &addr_count : *count_map) {
    ProfileMaps *maps = GetProfileMaps(addr_count.first + start);
    if (maps != NULL) {
      maps->address_count_map[addr_count.first + start] += addr_count.second;
    }
  }
  const RangeCountMap *range_map = &sample_reader_->range_count_map();
  for (const auto &range_count : *range_map) {
    ProfileMaps *maps = GetProfileMaps(range_count.first.first + start);
    if (maps != NULL) {
      maps->range_count_map[make_pair(
          range_count.first.first + start,
          range_count.first.second + start)] += range_count.second;
    }
  }
  const BranchCountMap *branch_map = &sample_reader_->branch_count_map();
  for (const auto &branch_count : *branch_map) {
    ProfileMaps *maps = GetProfileMaps(branch_count.first.first + start);
    if (maps != NULL) {
      maps->branch_count_map[make_pair(
          branch_count.first.first + start,
          branch_count.first.second + start)] += branch_count.second;
    }
  }
}

void Profile::ProcessPerFunctionProfile(string func_name,
                                        const ProfileMaps &maps) {
  if (sample_reader_->GetAggregateSampleCount(
      maps.start_addr - symbol_map_->base_addr(),
      maps.end_addr - symbol_map_->base_addr())
      <= sample_reader_->GetMaxCount() / FLAGS_sample_threshold) {
    return;
  }

  symbol_map_->AddSymbol(func_name);

  InstructionMap inst_map(addr2line_, symbol_map_);
  inst_map.BuildPerFunctionInstructionMap(func_name, maps.start_addr,
                                          maps.end_addr);

  AddressCountMap map;
  const AddressCountMap *map_ptr;
  if (FLAGS_use_lbr) {
    if (maps.range_count_map.size() == 0) {
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
    if (info == NULL) {
      continue;
    }
    const string *symbol;
    if (!symbol_map_->GetSymbolInfoByAddr(address_count.first, &symbol,
                                          NULL, NULL)) {
      continue;
    }
    bool is_in_head = symbol_map_->GetSymbolNameByStartAddr(
        address_count.first) != NULL;
    if (is_in_head) {
      symbol_map_->AddSymbolEntryCount(*symbol, address_count.second);
    }
    if (info->source_stack.size() > 0) {
      symbol_map_->AddSourceCount(func_name, info->source_stack,
                                  address_count.second, 0, SymbolMap::MAX);
    }
  }

  for (const auto &branch_count : maps.branch_count_map) {
    InstructionMap::InstMap::const_iterator iter =
        inst_map.inst_map().find(branch_count.first.first);
    if (iter == inst_map.inst_map().end()) {
      continue;
    }
    const InstructionMap::InstInfo *info = iter->second;
    if (info == NULL) {
      continue;
    }
    const string *callee = symbol_map_->GetSymbolNameByStartAddr(
        branch_count.first.second);
    if (!callee) {
      continue;
    }
    symbol_map_->AddIndirectCallTarget(func_name, info->source_stack,
                                       *callee, branch_count.second);
  }

  for (const auto &addr_count : *map_ptr) {
    global_addr_count_map_[addr_count.first] = addr_count.second;
  }
}

void Profile::ComputeProfile() {
  AggregatePerFunctionProfile();
  // Traverse the symbol map to process the profiles.
  for (const auto &symbol_profile : symbol_profile_maps_) {
    ProcessPerFunctionProfile(symbol_profile.first, *symbol_profile.second);
  }
  symbol_map_->Merge();
  symbol_map_->ComputeWorkingSets();
}

Profile::~Profile() {
  for (auto &symbol_maps : symbol_profile_maps_) {
    delete symbol_maps.second;
  }
}
}  // namespace autofdo
