// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Create AutoFDO Profile.

#include "profile_creator.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <memory>
#include <string>
#include <vector>

#include "addr2line.h"
#include "gcov.h"
#include "profile.h"
#include "sample_reader.h"
#include "source_info.h"
#include "symbol_map.h"
#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/logging.h"
#include "third_party/abseil/absl/container/btree_map.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/memory/memory.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "util/symbolize/elf_reader.h"

#if defined(HAVE_LLVM)
#include "llvm_profile_writer.h"
#include "profile_symbol_list.h"
#include "spe_sample_reader.h"

#include "llvm/ProfileData/SampleProf.h"

ABSL_FLAG(bool, disassemble_arm_branches, true,
          "Tell the SPE sample reader to disassemble the binary to identify "
          "branch instructions.");
AUTOFDO_PROFILE_SYMBOL_LIST_FLAGS;
#endif

ABSL_FLAG(std::string, focus_binary_re, "",
          "RE for the focused binary file name");

namespace {
struct PrefetchHint {
  uint64_t address;
  int64_t delta;
  std::string type;
};

typedef std::vector<PrefetchHint> PrefetchHints;

// Experimental support for providing cache prefetch hints.
// Currently, the format is a simple csv format, with no superfluous spaces or
// markers (e.g. quotes).
// Each line has 3 values:
//   - a PC (hex), corresponding to a memory operation in the profiled binary
//     (i.e. load/store);
//   - a delta (signed, base 10); and
//   - the hint type (string) - e.g. NTA, T{0|1|2}.
// The delta is to the next cache miss, calculated from the memory location
// used in the memory operation at PC.
PrefetchHints ReadPrefetchHints(const std::string &file_name) {
  PrefetchHints hints;
  FILE *fp = fopen(file_name.c_str(), "r");
  if (fp == nullptr) {
    LOG(ERROR) << "Cannot open " << file_name << "to read";
  } else {
    uint64_t address = 0;
    int64_t delta = 0;
    char prefetch_type[5];
    while (!feof(fp)) {
      if (3 != fscanf(fp, "%" SCNx64 ",%" SCNd64 ",%s\n", &address, &delta,
                      prefetch_type)) {
        LOG(ERROR) << "Error reading from " << file_name;
        break;
      }
      // We don't validate the type here. We'll just use it as a suffix to the
      // "__prefetch" indirect call, and let the compiler decide if it
      // supports it.
      std::string type(prefetch_type);
      hints.push_back({address, delta, type});
    }
    fclose(fp);
  }
  return hints;
}
}  // namespace

namespace devtools_crosstool_autofdo {
uint64_t ProfileCreator::GetTotalCountFromTextProfile(
    absl::string_view input_profile_name) {
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
  symbol_map->set_addr2line(absl::WrapUnique(addr2line));
  return true;
}

bool ProfileCreator::CreateProfile(const std::string &input_profile_name,
                                   const std::string &profiler,
                                   ProfileWriter *writer,
                                   const std::string &output_profile_name,
                                   bool store_sym_list_in_profile,
                                   bool check_lbr_entry) {
  SymbolMap symbol_map(binary_);

  writer->setSymbolMap(&symbol_map);
  if (profiler == "prefetch") {
    symbol_map.set_ignore_thresholds(true);
    if (!ConvertPrefetchHints(input_profile_name, &symbol_map)) return false;
  } else {
    if (!ReadSample(input_profile_name, profiler)) return false;
    symbol_map.ReadLoadableExecSegmentInfo(IsKernelSample());
    if (!ComputeProfile(&symbol_map, check_lbr_entry)) return false;
  }

#if defined(HAVE_LLVM)
  // Create prof_sym_list after symbol_map is populated because prof_sym_list
  // is expected not to contain any symbol showing up in the profile in
  // symbol_map.
  std::unique_ptr<llvm::sampleprof::ProfileSymbolList> prof_sym_list;
  NameSizeList name_size_list;
  if (store_sym_list_in_profile) {
    prof_sym_list = std::make_unique<llvm::sampleprof::ProfileSymbolList>();
    name_size_list = symbol_map.collectNamesForProfSymList();
    fillProfileSymbolList(prof_sym_list.get(), name_size_list, &symbol_map,
                          absl::GetFlag(FLAGS_symbol_list_size_coverage_ratio));
    prof_sym_list->setToCompress(absl::GetFlag(FLAGS_compress_symbol_list));
    auto *llvm_profile_writer = static_cast<LLVMProfileWriter *>(writer);
    auto *sample_profile_writer =
        llvm_profile_writer->CreateSampleWriter(output_profile_name);
    if (!sample_profile_writer) return false;
    sample_profile_writer->setProfileSymbolList(prof_sym_list.get());
  }
#endif

  return writer->WriteToFile(output_profile_name);
}

bool ProfileCreator::ReadSample(absl::string_view input_profile_name,
                                const std::string &profiler) {
  if (profiler == "perf" || profiler == "perf_spe") {
    std::string focus_binary_re;
    std::string build_id;
    // Sets the regular expression to filter samples for a given binary.
    if (!absl::GetFlag(FLAGS_focus_binary_re).empty()) {
      focus_binary_re = absl::GetFlag(FLAGS_focus_binary_re);
    } else {
      char *dup_name = strdup(binary_.c_str());
      char *strip_ptr = strstr(dup_name, ".unstripped");
      if (strip_ptr) {
        *strip_ptr = 0;
      }
      const char *file_base_name = basename(dup_name);
      CHECK(file_base_name) << "Cannot find basename for: " << binary_;
      focus_binary_re = std::string(".*/") + file_base_name + "$";
      free(dup_name);

      ElfReader reader(binary_);
      // Quipper (and other parts of google3's perf infrastructure) pads build
      // ids--if present--to 40 characters hex. Match that behavior here. See
      // quipper/perf_data_utils.h and b/21597512 for more info.
      const size_t kMinPerfBuildIDStringLength = 40;
      build_id = reader.GetBuildId();
      if (!build_id.empty() && build_id.length() < kMinPerfBuildIDStringLength)
        build_id.resize(kMinPerfBuildIDStringLength, '0');
    }

    if (profiler == "perf_spe") {
      #if defined (HAVE_LLVM)
      sample_reader_ = new PerfSpeDataSampleReader(
          absl::GetFlag(FLAGS_disassemble_arm_branches)
              ? PerfSpeDataSampleReader::BranchIdStrategy::kDisassembly
              : PerfSpeDataSampleReader::BranchIdStrategy::kSampling,
          input_profile_name, focus_binary_re, binary_);
      #else
      LOG(ERROR) << "Unsupported profiler type: " << profiler;
      return false;
      #endif // HAVE_LLVM
    } else {
      sample_reader_ = new PerfDataSampleReader(input_profile_name,
                                                focus_binary_re, build_id);
    }
  } else if (profiler == "text") {
    sample_reader_ = new TextSampleReaderWriter(input_profile_name);
  } else {
    LOG(ERROR) << "Unsupported profiler type: " << profiler;
    return false;
  }
  if (!sample_reader_->ReadAndSetTotalCount()) {
    LOG(ERROR) << "Error reading profile from " << input_profile_name;
    return false;
  }
  return true;
}
bool ProfileCreator::ComputeProfile(SymbolMap *symbol_map,
                                    bool check_lbr_entry) {
  if (!CheckAndAssignAddr2Line(symbol_map, Addr2line::Create(binary_)))
    return false;
  Profile profile(sample_reader_, binary_, symbol_map->get_addr2line(),
                  symbol_map);
  profile.ComputeProfile(check_lbr_entry);
  return true;
}

bool ProfileCreator::ConvertPrefetchHints(const std::string &profile_file,
                                          SymbolMap *symbol_map) {
  // Explicitly request constructing an Addr2line object with no sample profile
  // data. Otherwise, passing an empty sample profile map would elide all
  // addresses in the binary file, when the Google3Addr2line implementation is
  // used.
  if (!CheckAndAssignAddr2Line(symbol_map, Addr2line::Create(binary_)))
    return false;
  PrefetchHints hints = ReadPrefetchHints(profile_file);
  absl::btree_map<uint64_t, uint8_t> repeated_prefetches_indices;
  for (auto &hint : hints) {
    uint64_t pc = hint.address;
    int64_t delta = hint.delta;
    const std::string *name = nullptr;
    if (!symbol_map->GetSymbolInfoByAddr(pc, &name, nullptr, nullptr)) {
      LOG(INFO) << "Instruction address not found:" << std::hex << pc;
      continue;
    }
    uint8_t prefetch_index = repeated_prefetches_indices[pc]++;

    SourceStack stack;
    symbol_map->get_addr2line()->GetInlineStack(pc, &stack);

    if (!symbol_map->EnsureEntryInFuncForSymbol(*name, pc)) continue;

    // Currently, the profile format expects unsigned values, corresponding to
    // number of collected samples. We're hacking support for prefetch hints on
    // top of that, and prefetch hints are signed. For now, we'll explicitly
    // cast to unsigned.
    if (!symbol_map->AddIndirectCallTarget(
            *name, stack,
            "__prefetch_" + hint.type + "_" + std::to_string(prefetch_index),
            static_cast<uint64_t>(delta))) {
      LOG(WARNING) << "Ignoring address " << std::hex << pc
                   << ". Could not add an indirect call target. Likely the "
                      "inline stack is empty for this address.";
    }
  }
  symbol_map->ElideSuffixesAndMerge();
  return true;
}

uint64_t ProfileCreator::TotalSamples() {
  if (sample_reader_ == nullptr) {
    return 0;
  } else {
    return sample_reader_->GetTotalSampleCount();
  }
}

bool MergeSample(const std::vector<std::string> &input_files,
                 const std::string &input_profiler, absl::string_view binary,
                 absl::string_view output_file) {
  TextSampleReaderWriter writer(output_file);
  if (writer.IsFileExist()) {
    if (!writer.ReadAndSetTotalCount()) {
      return false;
    }
  }

  LOG(INFO) << "Merging " << input_files.size() << " " << input_profiler
            << " files";
  ProfileCreator creator(binary);
  for (const auto &input_file : input_files) {
    // It's easy to put e.g. trailing comma when making the list of inputs.
    // Skip any empty entries.
    if (input_file.empty()) continue;

    LOG_EVERY_N(INFO, 100) << "Merging " << input_file;
    if (!creator.ReadSample(input_file, input_profiler)) return false;
    writer.Merge(creator.sample_reader());
  }
  LOG(INFO) << "Merged " << input_files.size() << " " << input_profiler
            << " files";
  return writer.Write(nullptr);
}
}  // namespace devtools_crosstool_autofdo
