#ifndef AUTOFDO_PERFDATA_READER_H_
#define AUTOFDO_PERFDATA_READER_H_

#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lbr_aggregation.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_perf_data_provider.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "quipper/perf_reader.h"
#include "status_macros.h"

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
// filename against "match_mmap_name".
//
// `binary_content` contains static information and `perf_data` contains dynamic
// mmap information. The former is extracted from the binary's file name. the
// latter from perf mmap events by matching mmap's filename with
// "match_mmap_name".
//
// "binary_file_name" and "match_mmap_name" can be different, the former is
// the file name on the file system that we can read, the latter is the file
// name captured by perf, and it can reside on a deployed server. For example,
// binary_file_names can be "build/libxxx.so", whereas the match_mmap_names
// can be "/deployed/opt/release/runtime/libxxx_released.so".
//
// When match_mmap_name is "", `SelectMMaps` will use the
// build-id name, if build id is present, otherwise, it falls back to use
// binary_file_name.
absl::StatusOr<BinaryMMaps> SelectMMaps(
    PerfDataProvider::BufferHandle &perf_data,
    absl::string_view match_mmap_name, const BinaryContent &binary_content);

class PerfDataReader {
 public:
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

  // Parse LBR events that are matched by mmaps in perf_parse and store the data
  // in the aggregated counters.
  void AggregateLBR(LbrAggregation *result) const;

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
