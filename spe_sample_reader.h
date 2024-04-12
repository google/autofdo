#ifndef AUTOFDO_SPE_SAMPLE_READER_H_
#define AUTOFDO_SPE_SAMPLE_READER_H_

#include <sys/types.h>

#include <regex>  // NOLINT
#include <string>

#include "branch_frequencies_autofdo_sample.h"
#include "llvm_propeller_binary_content.h"
#include "perfdata_reader.h"
#include "sample_reader.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {

// An AutoFDO sample reader that reads in sample data from the output file of
// `perf record -e //arm_spe_0//`.
class PerfSpeDataSampleReader : public FileSampleReader {
 public:
  // A strategy to identify if an address is the PC of a branch instruction.
  // For a given virtual address, the `kDisassembly` strategy disassembles the
  // instruction at the address within the binary and checks if its opcode is a
  // branch. The `kSampling` strategy checks if the address was ever sampled as
  // a taken or not-taken branch in the SPE data.
  enum class BranchIdStrategy { kDisassembly, kSampling };

  PerfSpeDataSampleReader(BranchIdStrategy strategy,
                          absl::string_view profile_file,
                          absl::string_view mmap_regex,
                          absl::string_view binary_file);

  // This type is neither copyable nor movable.
  PerfSpeDataSampleReader(const PerfSpeDataSampleReader &) = delete;
  PerfSpeDataSampleReader &operator=(const PerfSpeDataSampleReader &) = delete;

  bool Append(const std::string &profile_file) override;

  // Reads the SPE records from the perf profile file and computes an
  // AutofdoRangeSample profile.
  absl::StatusOr<AutofdoRangeSample> GenerateAutofdoProfile(
      absl::string_view profile_file);

 protected:
  // Creates the PerfDataReader to parse the perf data contained in
  // `profile_file`.
  absl::StatusOr<PerfDataReader> CreatePerfDataReader(
      absl::string_view profile_file,
      const BinaryContent &binary_content) const;

  // Adds the branch and range counts in `sample` to `branch_count_map_` and
  // `range_count_map_`.
  void AddBranchesAndRanges(const AutofdoRangeSample &sample);

 private:
  // The strategy for identifying whether an address is a branch or not.
  const BranchIdStrategy branch_id_strategy_;
  // The regex to match perf mmaps.
  const std::regex mmap_regex_;
  // The name of the binary file from which the profile was recorded.
  const std::string binary_file_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_SPE_SAMPLE_READER_H_
