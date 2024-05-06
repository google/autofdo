#ifndef AUTOFDO_PERFDATA_READER_H_
#define AUTOFDO_PERFDATA_READER_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "branch_frequencies.h"
#include "lbr_aggregation.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_perf_data_provider.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "base/logging.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "third_party/abseil/absl/types/span.h"
#include "quipper/arm_spe_decoder.h"
#include "quipper/perf_reader.h"

namespace devtools_crosstool_autofdo {

// A memory mapping entry contains load start address, load size, page
// offset and file name.
struct MMapEntry {
  MMapEntry(uint32_t pid, uint64_t addr, uint64_t size, uint64_t pgoff,
            absl::string_view fn)
      : pid(pid),
        load_addr(addr),
        load_size(size),
        page_offset(pgoff),
        file_name(fn) {}
  const uint32_t pid;
  const uint64_t load_addr;
  const uint64_t load_size;
  const uint64_t page_offset;
  const std::string file_name;

  bool operator<(const MMapEntry &other) const {
    if (load_addr != other.load_addr) return load_addr < other.load_addr;
    if (load_size != other.load_size) return load_size < other.load_size;
    if (page_offset != other.page_offset)
      return page_offset < other.page_offset;
    if (file_name != other.file_name) return file_name < other.file_name;
    return false;
  }

  bool operator==(const MMapEntry &other) const {
    return load_addr == other.load_addr && load_size == other.load_size &&
           page_offset == other.page_offset && file_name == other.file_name;
  }

  std::string DebugString() const {
    return absl::StrFormat("[%#x, %#x](pgoff=%#x, size=%#x, fn='%s')",
                           load_addr, load_addr + load_size, page_offset,
                           load_size, file_name);
  }
};

// MMaps indexed by pid. (pid has to be uint32_t to be consistent with
// quippers's pid type.)
using BinaryMMaps = std::map<uint32_t, std::set<MMapEntry>>;

// Returns the set of file names with profiles in `perf_reader` with build IDs
// matching `build_id`.
absl::StatusOr<absl::flat_hash_set<std::string>> GetBuildIdNames(
    const quipper::PerfReader &perf_reader, absl::string_view build_id);

// Selects mmap events from perfdata file by comparing the mmap event's
// filename against "match_mmap_names".
//
// `binary_content` contains static information and `perf_data` contains dynamic
// mmap information. The former is extracted from the binary's file name. the
// latter from perf mmap events by matching mmap's filename with
// "match_mmap_names".
//
// "binary_file_name" and the contents of "match_mmap_names" can be different;
// the former is the file name on the file system that we can read, the latter
// is the file name captured by perf, and it can reside on a deployed server.
// For example, binary_file_names can be "build/libxxx.so", whereas the
// match_mmap_names can be "/deployed/opt/release/runtime/libxxx_released.so".
//
// When match_mmap_names is empty, `SelectMMaps` will use the
// build-id name, if the build id is present. Otherwise, it fails.
absl::StatusOr<BinaryMMaps> SelectMMaps(
    PerfDataProvider::BufferHandle &perf_data,
    absl::Span<const absl::string_view> match_mmap_names,
    const BinaryContent &binary_content);

class PerfDataReader {
 public:
  // The PID for mmaps belonging to the kernel.
  static constexpr uint32_t kKernelPid = static_cast<uint32_t>(-1);

  // Does not take ownership of `binary_content` which must refer to a valid
  // object that outlives the one constructed.
  PerfDataReader(PerfDataProvider::BufferHandle perf_data,
                 BinaryMMaps binary_mmaps, const BinaryContent *binary_content)
      : perf_data_(std::move(perf_data)),
        binary_mmaps_(std::move(binary_mmaps)),
        binary_content_(binary_content) {}
  PerfDataReader(const PerfDataReader &) = delete;
  PerfDataReader &operator=(const PerfDataReader &) = delete;
  PerfDataReader(PerfDataReader &&) = default;
  PerfDataReader &operator=(PerfDataReader &&) = default;

  // Reads the profile and applies the `callback` function on each sample event.
  // Only applies `callback` on events matching some mmap in `binary_mmaps_`.
  void ReadWithSampleCallBack(
      absl::FunctionRef<void(const quipper::PerfDataProto_SampleEvent &)>
          callback) const;

  // Reads the profile and applies the `callback` function on each SPE record.
  absl::Status ReadWithSpeRecordCallBack(
      absl::FunctionRef<void(const quipper::ArmSpeDecoder::Record &, int)>
          callback) const;

  // Parses LBR events that are matched by mmaps in perf_parse and stores the
  // data in the aggregated counters.
  void AggregateLBR(LbrAggregation *result) const;

  // Parses SPE events that are matched by mmaps in perf_parse and merges the
  // branch data with the branch frequencies in `result`.
  absl::Status AggregateSpe(BranchFrequencies &result) const;

  // "binary address" vs. "runtime address":
  //   binary address:  the address we get from "nm -n" or "readelf -s".
  //   runtime address: the address we get from perf data file.
  // The address in perfdata file is runtime address, for pie binaries, runtime
  // address must be translated to binary address (so we can map it to symbols),
  // for non-pie binaries, runtime address always equals to binary address.
  // Parameters:
  //    pid:  process id
  //   addr:  runtime address, as is from perf data
  uint64_t RuntimeAddressToBinaryAddress(uint32_t pid, uint64_t addr) const;

  const BinaryMMaps &binary_mmaps() const { return binary_mmaps_; }
  const PerfDataProvider::BufferHandle &perf_data() const { return perf_data_; }

  // Returns if the perf data was collected in kernel mode.
  bool IsKernelMode() const;

 private:
  PerfDataProvider::BufferHandle perf_data_;
  BinaryMMaps binary_mmaps_;
  const BinaryContent *binary_content_;
};

// Returns a `PerfDataReader` for profile represented by `perf_data` and
// binary represented by `binary_content`. Will use binary name matching
// instead of build-id if `match_mmap_name` is not empty.
// `binary_content` which must refer to a valid object that outlives the
// constructed object.
absl::StatusOr<PerfDataReader> BuildPerfDataReader(
    PerfDataProvider::BufferHandle perf_data,
    const BinaryContent *binary_content, absl::string_view match_mmap_name);

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_PERFDATA_READER_H_
