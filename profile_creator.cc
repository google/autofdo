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

// Create AutoFDO Profile.

#include <memory>

#include "addr2line.h"
#include "base/common.h"
#include "gcov.h"
#include "gflags/gflags.h"
#include "module_grouper.h"
#include "profile.h"
#include "profile_creator.h"
#include "profile_writer.h"
#include "sample_reader.h"
#include "symbol_map.h"
#include "symbolize/elf_reader.h"

namespace {
struct PrefetchHint {
  uint64 address;
  int64 delta;
  string type;
};

typedef std::vector<PrefetchHint> PrefetchHints;

// Experimental support for providing cache prefetch hints.
// Currently, the format is a simple csv format, with no superfluous spaces or
// markers (e.g. quotes).
// Each line has 3 values:
//   - a PC (hex), corresponding to a memory operation in the profiled binary
//     (i.e. load/store);
//   - a delta (signed, base 10); and
//   - the hint type (lower case string) - e.g. nta, t{0|1|2}.
// The delta is to the next cache miss, calculated from the memory location
// used in the memory operation at PC.
PrefetchHints ReadPrefetchHints(const string &file_name) {
  PrefetchHints hints;
  FILE *fp = fopen(file_name.c_str(), "r");
  if (fp == nullptr) {
    LOG(ERROR) << "Cannot open " << file_name << "to read";
  } else {
    uint64 address = 0;
    int64 delta = 0;
    char prefetch_type[5];
    while (!feof(fp)) {
      if (3 != fscanf(fp, "%llx,%lld,%s\n", &address, &delta, prefetch_type)) {
        LOG(ERROR) << "Error reading from " << file_name;
        break;
      }
      // We don't validate the type here. We'll just use it as a suffix to the
      // "__prefetch" indirect call, and let the compiler decide if it
      // supports it.
      string type(prefetch_type);
      hints.push_back({address, delta, type});
    }
    fclose(fp);
  }
  return hints;
}
}  // namespace
namespace autofdo {
uint64 ProfileCreator::GetTotalCountFromTextProfile(
    const string &input_profile_name) {
  ProfileCreator creator("");
  if (!creator.ReadSample(input_profile_name, "text")) {
    return 0;
  }
  return creator.TotalSamples();
}

bool ProfileCreator::CheckAndAssignAddr2Line(SymbolMap *symbol_map,
                                             Addr2line *addr2line) {
  if (addr2line == nullptr) {
    LOG(ERROR) << "Error reading binary " << binary_;
    return false;
  }
  symbol_map->set_addr2line(std::unique_ptr<Addr2line>(addr2line));
  return true;
}

bool ProfileCreator::CreateProfile(const string &input_profile_name,
                                   const string &profiler,
                                   ProfileWriter *writer,
                                   const string &output_profile_name) {
  SymbolMap symbol_map(binary_);
  symbol_map.set_use_discriminator_encoding(use_discriminator_encoding_);
  auto grouper =
      ModuleGrouper::GroupModule(binary_, GCOV_ELF_SECTION_NAME, &symbol_map);

  writer->setSymbolMap(&symbol_map);
  writer->setModuleMap(&grouper->module_map());
  if (profiler == "prefetch") {
    symbol_map.set_ignore_thresholds(true);
    if (!ConvertPrefetchHints(input_profile_name, &symbol_map)) return false;
  } else {
    if (!ReadSample(input_profile_name, profiler)) return false;
    if (!ComputeProfile(&symbol_map)) return false;
  }
  bool ret = writer->WriteToFile(output_profile_name);
  return ret;
}

bool ProfileCreator::ReadSample(const string &input_profile_name,
                                const string &profiler) {
  if (profiler == "perf") {
    string file_base_name = basename(binary_.c_str());
    size_t unstripped_at = file_base_name.find(".unstripped");
    if (unstripped_at != string::npos) {
      file_base_name.erase(unstripped_at);
    }
    CHECK(!file_base_name.empty()) << "Cannot find basename for: " << binary_;

    ElfReader reader(binary_);

    sample_reader_ =
        new PerfDataSampleReader(input_profile_name, std::move(file_base_name));
  } else if (profiler == "text") {
    sample_reader_ = new TextSampleReaderWriter(input_profile_name);
  } else {
    LOG(ERROR) << "Unsupported profiler type: " << profiler;
    return false;
  }
  if (!sample_reader_->ReadAndSetTotalCount()) {
    LOG(ERROR) << "Error reading profile.";
    return false;
  }
  return true;
}
bool ProfileCreator::ComputeProfile(SymbolMap *symbol_map) {
  std::set<uint64> sampled_addrs = sample_reader_->GetSampledAddresses();
  std::map<uint64, uint64> sampled_functions =
      symbol_map->GetSampledSymbolStartAddressSizeMap(sampled_addrs);
  if (!CheckAndAssignAddr2Line(
          symbol_map,
          Addr2line::CreateWithSampledFunctions(binary_, &sampled_functions)))
    return false;
  Profile profile(sample_reader_, binary_, symbol_map->get_addr2line(),
                  symbol_map);
  profile.ComputeProfile();

  return true;
}

bool ProfileCreator::ConvertPrefetchHints(const string &profile_file,
                                          SymbolMap *symbol_map) {
  // Explicitly request constructing an Addr2line object with no sample profile
  // data. Otherwise, passing an empty sample profile map would elide all
  // addresses in the binary file, when the Google3Addr2line implementation is
  // used.
  if (!CheckAndAssignAddr2Line(symbol_map, Addr2line::Create(binary_)))
    return false;
  PrefetchHints hints = ReadPrefetchHints(profile_file);
  std::map<uint64, uint8> repeated_prefetches_indices;
  for (auto &hint : hints) {
    uint64 pc = hint.address;
    int64 delta = hint.delta;
    const string *name = nullptr;
    if (!symbol_map->GetSymbolInfoByAddr(pc, &name, nullptr, nullptr)) {
      LOG(INFO) << "Instruction address not found:" << std::hex << pc;
      continue;
    }
    uint8 prefetch_index = repeated_prefetches_indices[pc]++;

    SourceStack stack;
    symbol_map->get_addr2line()->GetInlineStack(pc, &stack);

    if (symbol_map->map().find(*name) == symbol_map->map().end()) {
      symbol_map->AddSymbol(*name);
      // Add bogus samples, so that the writer won't skip over.
      if (!symbol_map->TraverseInlineStack(*name, stack,
                                           symbol_map->count_threshold() + 1)) {
        LOG(WARNING) << "Ignoring address " << std::hex << pc
                     << ". No inline stack found.";
        continue;
      }
    }

    // Currently, the profile format expects unsigned values, corresponding to
    // number of collected samples. We're hacking support for prefetch hints on
    // top of that, and prefetch hints are signed. For now, we'll explicitly
    // cast to unsigned.
    if (!symbol_map->AddIndirectCallTarget(
            *name, stack,
            "__prefetch_" + hint.type + "_" + std::to_string(prefetch_index),
            static_cast<uint64>(delta))) {
      LOG(WARNING) << "Ignoring address " << std::hex << pc
                   << ". Could not add an indirect call target. Likely the "
                      "inline stack is empty for this address.";
    }
  }
  return true;
}

uint64 ProfileCreator::TotalSamples() {
  if (sample_reader_ == nullptr) {
    return 0;
  } else {
    return sample_reader_->GetTotalSampleCount();
  }
}

bool MergeSample(const string &input_file, const string &input_profiler,
                 const string &binary, const string &output_file) {
  TextSampleReaderWriter writer(output_file);
  if (writer.IsFileExist()) {
    if (!writer.ReadAndSetTotalCount()) {
      return false;
    }
  }

  ProfileCreator creator(binary);
  if (creator.ReadSample(input_file, input_profiler)) {
    writer.Merge(creator.sample_reader());
    if (writer.Write(nullptr)) {
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}
}  // namespace autofdo
