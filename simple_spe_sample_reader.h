#ifndef AUTOFDO_SIMPLE_SPE_SAMPLE_READER_H_
#define AUTOFDO_SIMPLE_SPE_SAMPLE_READER_H_

#include <filesystem>
#include <string>
#include "sample_reader.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {

// Simplified ARM SPE sample reader for GCOV mode (no LLVM dependencies).
//
// This class provides ARM SPE profiling support for create_gcov without
// requiring LLVM libraries. It complements PerfSpeDataSampleReader (LLVM mode)
// by offering a lightweight alternative optimized for GCOV profile generation.
//
// Why a separate class?
// PerfSpeDataSampleReader has deep LLVM dependencies (BinaryContent,
// MiniDisassembler, PerfDataProvider) that cannot be easily removed without
// extensive conditional compilation and loss of functionality. Separate classes
// provide clean separation, each optimized for its use case.
//
// How it works:
// 1. ARM SPE hardware records branch information during execution (is_br_eret,
//    br_cond, cond_not_taken flags) and target addresses.
// 2. quipper::ArmSpeDecoder decodes the SPE binary format from perf.data.
// 3. MMAP events from perf.data are used to convert runtime virtual addresses
//    to binary text section offsets for symbol resolution.
// 4. Execution ranges are synthesized from branch data using incoming branch
//    weights and forward scanning (similar to LLVM's ToRangeCountMap).
//
// Branch identification:
// Unlike PerfSpeDataSampleReader's kDisassembly strategy (which uses LLVM's
// MiniDisassembler to decode instructions), this class relies on the ARM SPE
// hardware's branch identification (is_br_eret flag). The hardware acts as a
// real-time disassembler, marking which instructions are branches during
// execution. This is equivalent to the kSampling strategy but simpler.
//
//
class SimpleSpeDataSampleReader : public FileSampleReader {
 public:
  SimpleSpeDataSampleReader(absl::string_view profile_file,
                             absl::string_view binary_file);

  // This type is neither copyable nor movable.
  SimpleSpeDataSampleReader(const SimpleSpeDataSampleReader &) = delete;
  SimpleSpeDataSampleReader &operator=(const SimpleSpeDataSampleReader &) = delete;

  bool Append(const std::string &profile_file) override;

 protected:
  // Reads ARM SPE data from perf.data. Parses MMAP events for address mapping,
  // decodes AUXTRACE events for branch data, converts virtual addresses to
  // binary offsets, and synthesizes execution ranges.
  bool Read() override;

 private:
  // Synthesizes execution ranges from branch data by finding incoming branch
  // targets and scanning forward. Populates range_count_map_ which is required
  // by Profile::ComputeProfile for symbol resolution.
  void SynthesizeRanges();
  
  const std::filesystem::path binary_file_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_SIMPLE_SPE_SAMPLE_READER_H_

