// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to represent source level profile.
#include "profile.h"

#include <cstdint>
#include <ios>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "instruction_map.h"
#include "mini_disassembler.h"
#include "sample_reader.h"
#include "source_info.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/Object/ObjectFile.h"

ABSL_FLAG(bool, use_lbr, true,
            "Whether to use lbr profile.");
ABSL_FLAG(bool, llc_misses, false, "The profile represents llc misses.");

namespace devtools_crosstool_autofdo {
Profile::ProfileMaps *Profile::GetProfileMaps(uint64_t addr) {
  const std::string *name;
  uint64_t start_addr, end_addr;
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

void Profile::AggregatePerFunctionProfile(bool check_lbr_entry) {
  const AddressCountMap *count_map = &sample_reader_->address_count_map();
  for (const auto &[addr, count] : *count_map) {
    uint64_t vaddr = symbol_map_->get_static_vaddr(addr);
    ProfileMaps *maps = GetProfileMaps(vaddr);
    if (maps != nullptr) {
      maps->address_count_map[vaddr] += count;
    }
  }

  std::unique_ptr<MiniDisassembler> Disassembler;
  if (check_lbr_entry) {
    const llvm::object::ObjectFile *Obj = addr2line_->getObject();
    if (Obj) {
      auto DisassemblerHandler = MiniDisassembler::Create(Obj);
      if (DisassemblerHandler.ok()) {
        Disassembler = std::move(DisassemblerHandler.value());
      }
    }
    if (!Disassembler) {
      LOG(WARNING) << "Cannot create MiniDisassembler and check lbr entry "
                   << " will be disabled.";
    }
  }

  const RangeCountMap *range_map = &sample_reader_->range_count_map();
  for (const auto &[range, count] : *range_map) {
    uint64_t beg_vaddr = symbol_map_->get_static_vaddr(range.first);
    uint64_t end_vaddr = symbol_map_->get_static_vaddr(range.second);
    ProfileMaps *maps = GetProfileMaps(beg_vaddr);

    // check if the range end_addr is a jump instruction.
    if (Disassembler) {
      auto BranchCheck = Disassembler->MayAffectControlFlow(end_vaddr);
      if (BranchCheck.ok() && !BranchCheck.value())
        LOG(WARNING)
            << "Range end_addr (" << std::hex << end_vaddr
            << ") is NOT a potentially-control-flow-affecting instruction.";
    }

    if (maps != nullptr) {
      maps->range_count_map[std::make_pair(beg_vaddr, end_vaddr)] += count;
    }
  }
  const BranchCountMap *branch_map = &sample_reader_->branch_count_map();
  for (const auto &[branch, count] : *branch_map) {
    uint64_t from_vaddr = symbol_map_->get_static_vaddr(branch.first);
    uint64_t to_vaddr = symbol_map_->get_static_vaddr(branch.second);
    ProfileMaps *maps = GetProfileMaps(from_vaddr);

    if (Disassembler) {
      auto BranchCheck = Disassembler->MayAffectControlFlow(from_vaddr);
      if (BranchCheck.ok() && !BranchCheck.value())
        LOG(WARNING)
            << "Branch from_addr (" << std::hex << from_vaddr
            << ") is NOT a potentially-control-flow-affecting instruction.";
    }

    if (maps != nullptr) {
      maps->branch_count_map[std::make_pair(from_vaddr, to_vaddr)] += count;
    }
  }

  // Add an entry for each symbol so that later we can decide if the hot and
  // cold parts together need to be emitted.
  for (const auto &[name, addr] : symbol_map_->GetNameAddrMap()) {
    CHECK(GetProfileMaps(addr));
  }
}

uint64_t Profile::ProfileMaps::GetAggregatedCount() const {
  uint64_t ret = 0;

  if (!range_count_map.empty()) {
    for (const auto &[range, count] : range_count_map) {
      ret += count * (1 + range.second - range.first);
    }
  } else {
    for (const auto &[addr, count] : address_count_map) {
      ret += count;
    }
  }
  return ret;
}

void Profile::ProcessPerFunctionProfile(absl::string_view func_name,
                                        const ProfileMaps &maps) {
  InstructionMap inst_map(addr2line_, symbol_map_);
  // LOG(INFO) << "ProcessPerFunctionProfile: " << func_name;
  inst_map.BuildPerFunctionInstructionMap(func_name, maps.start_addr,
                                          maps.end_addr);
  // LOG(INFO) << "Built instruction map for func: " << func_name;

  AddressCountMap map;
  const AddressCountMap *map_ptr;
  if (absl::GetFlag(FLAGS_use_lbr)) {
    if (maps.range_count_map.empty()) {
      LOG(WARNING) << "use_lbr was enabled but range_count_map was empty!";
      return;
    }
    for (const auto &[range, count] : maps.range_count_map) {
      for (uint64_t addr = range.first;
           inst_map.lookup(addr) && addr <= range.second; ++addr) {
        map[addr] += count;
      }
    }
    map_ptr = &map;
  } else {
    map_ptr = &maps.address_count_map;
  }

  for (const auto &[address, count] : *map_ptr) {
    const InstructionMap::InstInfo *info = inst_map.lookup(address);
    if (info == nullptr) {
      continue;
    }
    if (!info->source_stack.empty()) {
      // LOG(INFO) << "adding source count for func: " << func_name
      //           << " address: " << address;
      symbol_map_->AddSourceCount(func_name, info->source_stack, count, 0,
                                  info->source_stack[0].DuplicationFactor(),
                                  SymbolMap::PERFDATA);
    }
  }

  for (const auto &[branch, count] : maps.branch_count_map) {
    const InstructionMap::InstInfo *info = inst_map.lookup(branch.first);
    if (info == nullptr) {
      continue;
    }
    const std::string *callee =
        symbol_map_->GetSymbolNameByStartAddr(branch.second);
    if (!callee) {
      continue;
    }
    if (symbol_map_->map().count(*callee)) {
      symbol_map_->AddSymbolEntryCount(*callee, count);
      symbol_map_->AddIndirectCallTarget(func_name, info->source_stack, *callee,
                                         count, SymbolMap::PERFDATA);
    }
  }

  for (const auto &[addr, count] : *map_ptr) {
    global_addr_count_map_[addr] = count;
  }
}

void Profile::ComputeProfile(bool check_lbr_entry) {
  symbol_map_->CalculateThresholdFromTotalCount(
      sample_reader_->GetTotalCount());
  AggregatePerFunctionProfile(check_lbr_entry);

  if (absl::GetFlag(FLAGS_llc_misses)) {
    for (const auto &[func_name, maps] : symbol_profile_maps_) {
      std::map<uint64_t, uint64_t> counts;
      for (const auto &[pc, count] : maps->address_count_map) {
        DCHECK(maps->start_addr <= pc && pc <= maps->end_addr);
        if (!symbol_map_->EnsureEntryInFuncForSymbol(func_name, pc))
          continue;
        counts[pc] += count;
      }

      CHECK(maps->branch_count_map.empty());
      for (const auto &[pc, count] : counts) {
        SourceStack stack;
        // LOG(INFO) << "getting inline stack for pc: " << pc;
        symbol_map_->get_addr2line()->GetInlineStack(pc, &stack);
        symbol_map_->AddIndirectCallTarget(func_name, stack, "__llc_misses__",
                                           count);
      }
    }
    symbol_map_->ElideSuffixesAndMerge();
  } else {
    // Precompute the aggregated counts for all split parts. All function parts
    // are emitted only if their total sample count is above the required
    // threshold.
    absl::flat_hash_map<std::string, uint64_t> symbol_counts;
    for (const auto &[name, profile] : symbol_profile_maps_) {
      symbol_counts[symbol_map_->GetOriginalName(name)] +=
          profile->GetAggregatedCount();
    }

    // First add all symbols that needs to be outputted to the symbol_map_. We
    // need to do this before hand because ProcessPerFunctionProfile will call
    // AddSymbolEntryCount for other symbols, which may or may not had been
    // processed by ProcessPerFunctionProfile.
    for (const auto &[name, ignored] : symbol_profile_maps_) {
      const uint64_t count =
          symbol_counts.at(symbol_map_->GetOriginalName(name));
      if (symbol_map_->ShouldEmit(count)) {
        symbol_map_->AddSymbol(name);
      }
    }

    for (const auto &[name, profile] : symbol_profile_maps_) {
      const uint64_t count =
          symbol_counts.at(symbol_map_->GetOriginalName(name));
      if (symbol_map_->ShouldEmit(count)) {
        ProcessPerFunctionProfile(name, *profile);
      }
    }
    symbol_map_->ElideSuffixesAndMerge();
    symbol_map_->ComputeWorkingSets();
  }
}

Profile::~Profile() {
  for (auto &[symbol, maps] : symbol_profile_maps_) {
    delete maps;
  }
}
}  // namespace devtools_crosstool_autofdo
