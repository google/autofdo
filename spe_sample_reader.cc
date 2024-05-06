#include "spe_sample_reader.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>  // NOLINT
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "branch_frequencies.h"
#include "branch_frequencies_autofdo_sample.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_perf_data_provider.h"
#include "mini_disassembler.h"
#include "perfdata_reader.h"
#include "sample_reader.h"
#include "symbol_map.h"
#include "base/logging.h"
#include "base/status_macros.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/base/nullability.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/TargetParser/Triple.h"
#include "perf_data.pb.h"
#include "quipper/perf_reader.h"

namespace devtools_crosstool_autofdo {
namespace {
// A functor that takes a binary address and returns whether or not the address
// is a branch. May return an error if the address is invalid.
class BranchIdentifier {
 public:
  virtual ~BranchIdentifier() = default;

  virtual absl::StatusOr<bool> operator()(int64_t addr) const = 0;
};

// A branch identifier that disassembles the instruction at the binary address
// to determine if it is a branch.
class DisassemblerBranchIdentifier : public BranchIdentifier {
 public:
  // Creates a disassembler branch identifier that disassembles the instructions
  // in `binary_content`.
  static absl::StatusOr<std::unique_ptr<DisassemblerBranchIdentifier>> Create(
      const BinaryContent& binary_content) {
    ASSIGN_OR_RETURN(
        std::unique_ptr<MiniDisassembler> disassembler,
        MiniDisassembler::Create(binary_content.object_file.get()));

    return std::make_unique<DisassemblerBranchIdentifier>(
        std::move(disassembler));
  }

  explicit DisassemblerBranchIdentifier(
      absl::Nonnull<std::unique_ptr<MiniDisassembler>> disassembler)
      : disassembler_(std::move(disassembler)) {}

  // DisassemblerBranchIdentifier is neither copyable nor movable.
  DisassemblerBranchIdentifier(const DisassemblerBranchIdentifier&) = delete;
  DisassemblerBranchIdentifier& operator=(const DisassemblerBranchIdentifier&) =
      delete;

  absl::StatusOr<bool> operator()(int64_t addr) const override {
    return disassembler_->MayAffectControlFlow(addr);
  }

 private:
  absl::Nonnull<std::unique_ptr<MiniDisassembler>> disassembler_;
};

// A branch identifier that determines if an address is a branch or not by
// checking if there was a branch sample from the address.
class SampledBranchIdentifier : public BranchIdentifier {
 public:
  // Creates a sampled branch identifier from the addresses sampled as taken or
  // not-taken branches in `frequencies`.
  static absl::StatusOr<std::unique_ptr<SampledBranchIdentifier>> Create(
      const BranchFrequencies& frequencies) {
    absl::flat_hash_set<uint64_t> branch_instruction_addresses;
    for (const auto& [branch, count] : frequencies.taken_branch_counters) {
      branch_instruction_addresses.insert(branch.from);
    }
    for (const auto& [branch, count] : frequencies.not_taken_branch_counters) {
      branch_instruction_addresses.insert(branch.address);
    }
    return std::make_unique<SampledBranchIdentifier>(
        std::move(branch_instruction_addresses));
  }

  explicit SampledBranchIdentifier(
      absl::flat_hash_set<uint64_t> branch_addresses)
      : branch_addresses_(std::move(branch_addresses)) {}

  // SampledBranchIdentifier is neither copyable nor movable.
  SampledBranchIdentifier(const SampledBranchIdentifier&) = delete;
  SampledBranchIdentifier& operator=(const SampledBranchIdentifier&) = delete;

  absl::StatusOr<bool> operator()(int64_t addr) const override {
    return branch_addresses_.contains(addr);
  }

 private:
  absl::flat_hash_set<uint64_t> branch_addresses_;
};

absl::StatusOr<std::unique_ptr<BranchIdentifier>> CreateBranchIdentifier(
    PerfSpeDataSampleReader::BranchIdStrategy strategy,
    const BinaryContent& binary_content, const BranchFrequencies& frequencies) {
  switch (strategy) {
    case PerfSpeDataSampleReader::BranchIdStrategy::kDisassembly:
      return DisassemblerBranchIdentifier::Create(binary_content);
    case PerfSpeDataSampleReader::BranchIdStrategy::kSampling:
      return SampledBranchIdentifier::Create(frequencies);
  }
}

// Checks if a binary is AArch64.
bool IsAArch64(const BinaryContent& binary_content) {
  return binary_content.object_file != nullptr &&
         binary_content.object_file->getArch() ==
             llvm::Triple::ArchType::aarch64;
}

// Fetches the data for a perf profile.
absl::StatusOr<PerfDataProvider::BufferHandle> FetchPerfData(
    absl::string_view profile_file) {
  auto perf_data_provider = std::make_unique<GenericFilePerfDataProvider>(
      std::vector{std::string(profile_file)});
  ASSIGN_OR_RETURN(std::optional<PerfDataProvider::BufferHandle> perf_data,
                   perf_data_provider->GetNext());
  if (!perf_data.has_value()) {
    return absl::InternalError(
        absl::StrCat("Failed to get perf data for profile ", profile_file));
  }

  return *std::move(perf_data);
}
}  // namespace

PerfSpeDataSampleReader::PerfSpeDataSampleReader(BranchIdStrategy strategy,
                                                 absl::string_view profile_file,
                                                 absl::string_view mmap_regex,
                                                 absl::string_view binary_file)
    : FileSampleReader(profile_file),
      branch_id_strategy_(strategy),
      mmap_regex_(std::string(mmap_regex)),
      binary_file_(binary_file) {}

absl::StatusOr<PerfDataReader> PerfSpeDataSampleReader::CreatePerfDataReader(
    absl::string_view profile_file, const BinaryContent& binary_content) const {
  // Fetch the perf data and determine the mmap names to look for within it.
  ASSIGN_OR_RETURN(PerfDataProvider::BufferHandle perf_data,
                   FetchPerfData(profile_file));

  std::vector<absl::string_view> match_mmap_names;
  // Only use the mmap name regex if there is no build_id.
  if (binary_content.build_id.empty()) {
    // Extract all filenames from mmap events.
    quipper::PerfReader reader;
    // Ignore the most common non-MMAP events to reduce memory usage.
    reader.SetEventTypesToSkipWhenSerializing(
        {quipper::PERF_RECORD_SAMPLE, quipper::PERF_RECORD_FORK,
         quipper::PERF_RECORD_COMM, quipper::PERF_RECORD_AUXTRACE});

    reader.ReadFromPointer(perf_data.buffer->getBufferStart(),
                           perf_data.buffer->getBufferSize());
    std::set<std::string> filenames;
    reader.GetFilenamesAsSet(&filenames);
    // Filter out filenames that don't match the regex.
    absl::c_copy_if(filenames, std::back_inserter(match_mmap_names),
                    [this](const std::string& filename) -> bool {
                      return std::regex_search(filename.c_str(), mmap_regex_);
                    });
  }

  ASSIGN_OR_RETURN(BinaryMMaps binary_mmaps,
                   SelectMMaps(perf_data, match_mmap_names, binary_content));
  return PerfDataReader(std::move(perf_data), std::move(binary_mmaps),
                        &binary_content);
}

bool PerfSpeDataSampleReader::Append(const std::string& profile_file) {
  absl::StatusOr<AutofdoRangeSample> autofdo_range_sample =
      GenerateAutofdoProfile(profile_file);
  if (!autofdo_range_sample.ok()) {
    LOG(ERROR) << autofdo_range_sample.status();
    return false;
  }

  AddBranchesAndRanges(*autofdo_range_sample);
  return true;
}

absl::StatusOr<AutofdoRangeSample>
PerfSpeDataSampleReader::GenerateAutofdoProfile(
    absl::string_view profile_file) {
  ASSIGN_OR_RETURN(std::unique_ptr<BinaryContent> binary_content,
                   GetBinaryContent(binary_file_));

  if (!IsAArch64(*binary_content)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Binary ", binary_content->file_name, " is not AArch64"));
  }

  ASSIGN_OR_RETURN(PerfDataReader perf_data_reader,
                   CreatePerfDataReader(profile_file, *binary_content));

  BranchFrequencies branch_frequencies;
  RETURN_IF_ERROR(perf_data_reader.AggregateSpe(branch_frequencies));

  SymbolMap symbol_map(binary_file_);
  symbol_map.ReadLoadableExecSegmentInfo(perf_data_reader.IsKernelMode());

  ASSIGN_OR_RETURN(std::unique_ptr<BranchIdentifier> branch_identifier,
                   CreateBranchIdentifier(branch_id_strategy_, *binary_content,
                                          branch_frequencies));

  return ConvertBranchFrequenciesToAutofdoRangeSample(
      branch_frequencies, *branch_identifier,
      [&symbol_map](int64_t addr) -> int64_t {
        return symbol_map.GetFileOffsetFromStaticVaddr(addr);
      },
      /*instruction_size=*/4);
}

void PerfSpeDataSampleReader::AddBranchesAndRanges(
    const AutofdoRangeSample& sample) {
  for (const auto& [branch, count] : sample.branch_counts) {
    branch_count_map_[branch] += count;
  }
  for (const auto& [range, count] : sample.range_counts) {
    range_count_map_[range] += count;
  }
}

}  // namespace devtools_crosstool_autofdo
