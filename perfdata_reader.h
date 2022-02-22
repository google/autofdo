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

#include "llvm_propeller_perf_data_provider.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "quipper/perf_parser.h"

namespace devtools_crosstool_autofdo {

// A memory mapping entry contains load start address, load size, page
// offset and file name.
struct MMapEntry {
  MMapEntry(uint64_t addr, uint64_t size, uint64_t pgoff, const std::string &fn)
      : load_addr(addr), load_size(size), page_offset(pgoff), file_name(fn) {}
  const uint64_t load_addr;
  const uint64_t load_size;
  const uint64_t page_offset;
  const std::string file_name;

  bool operator<(const MMapEntry &other) const {
    if (load_addr != other.load_addr) return load_addr < other.load_addr;
    if (load_size != other.load_size) return load_size < other.load_size;
    if (page_offset != other.page_offset)
      return page_offset < other.page_offset;
    if (file_name != other.file_name)
      return file_name < other.file_name;
    return false;
  }

  bool operator==(const MMapEntry &other) const {
    return load_addr == other.load_addr && load_size == other.load_size &&
           page_offset == other.page_offset && file_name == other.file_name;
  }
};

// BinaryInfo represents information for an ELF executable or a shared object,
// the data contained include (loadable) segments, file name, file content and
// DYN tag (is_pie).
struct BinaryInfo {
  struct Segment {
    uint64_t offset;
    uint64_t vaddr;
    uint64_t memsz;
  };

  std::string file_name;
  std::unique_ptr<llvm::MemoryBuffer> file_content = nullptr;
  std::unique_ptr<llvm::object::ObjectFile> object_file = nullptr;
  bool is_pie = false;
  std::vector<Segment> segments;
  std::string build_id;

  BinaryInfo(const BinaryInfo&) = delete;
  BinaryInfo() {}
  BinaryInfo(BinaryInfo &&bi)
      : file_name(std::move(bi.file_name)),
        file_content(std::move(bi.file_content)),
        object_file(std::move(bi.object_file)),
        is_pie(bi.is_pie),
        segments(std::move(bi.segments)),
        build_id(std::move(bi.build_id)) {}
};

// MMaps indexed by pid.
using BinaryMMaps = std::map<uint64_t, std::set<MMapEntry>>;

struct BinaryPerfInfo {
  BinaryMMaps binary_mmaps;
  BinaryInfo binary_info;
  std::optional<PerfDataProvider::BufferHandle> perf_data;

  BinaryPerfInfo() = default;
  BinaryPerfInfo(const BinaryPerfInfo &) = delete;
  BinaryPerfInfo(BinaryPerfInfo &&bpi) = default;

  void ResetPerfInfo() {
    perf_data.reset();
    binary_mmaps.clear();
  }
};

struct LBRAggregation {
  // <from_address, to_address> -> branch counter.
  // Note all addresses are binary addresses, not runtime addresses.
  using BranchCountersTy = std::map<std::pair<uint64_t, uint64_t>, uint64_t>;

  // <fallthrough_from, fallthrough_to> -> fallthrough counter.
  // Note all addresses are symbol address, not virtual addresses.
  using FallthroughCountersTy =
      std::map<std::pair<uint64_t, uint64_t>, uint64_t>;

  LBRAggregation() = default;
  ~LBRAggregation() = default;

  LBRAggregation(LBRAggregation &&o)
      : branch_counters(std::move(o.branch_counters)),
        fallthrough_counters(std::move(o.fallthrough_counters)) {}

  LBRAggregation &operator=(LBRAggregation &&o) {
    branch_counters = std::move(o.branch_counters);
    fallthrough_counters = std::move(o.fallthrough_counters);
    return *this;
  }

  LBRAggregation(const LBRAggregation &) = delete;
  LBRAggregation &operator=(const LBRAggregation &) = delete;

  // See BranchCountersTy.
  BranchCountersTy branch_counters;

  // See FallthroughCountersTy.
  FallthroughCountersTy fallthrough_counters;
};

class PerfDataReader {
 public:
  PerfDataReader() {}
  virtual ~PerfDataReader() {}

  virtual bool GetBuildIdNames(const quipper::PerfReader &perf_reader,
                               const std::string &buildid,
                               std::set<std::string> *buildid_names);

  bool SelectBinaryInfo(const std::string &binary_file_name,
                        BinaryInfo *binary_info) const;

  // Get BinaryPerfInfo from binary_file_name and perf data. The BinaryPerfInfo
  // contains static information and dynamic mmap information. The former is
  // extracted from the "binary_file_name", the latter from perf mmap events by
  // matching mmap's filename with "match_mmap_name".
  //
  // "binary_file_name" and "match_mmap_name" can be different, the former is
  // the file name on the file system that we can read, the latter is the file
  // name captured by perf, and it can reside on a deployed server. For example,
  // binary_file_names can be "build/libxxx.so", whereas the match_mmap_names
  // can be "/deployed/opt/release/runtime/libxxx_released.so".
  //
  // When match_mmap_name is "", SelectBinaryPerfInfo will automatically use the
  // build-id name, if build id is present, otherwise, it falls back to use
  // binary_file_name.
  bool SelectPerfInfo(PerfDataProvider::BufferHandle perf_data,
                      const std::string &match_mmap_name,
                      BinaryPerfInfo *binary_perf_info) const;

  // Parse LBR events that are matched by mmaps in perf_parse and store the data
  // in the aggregated counters.
  void AggregateLBR(const BinaryPerfInfo &binary_perf_info,
                    LBRAggregation *result) const;

  // "binary address" vs. "runtime address":
  //   binary address:  the address we get from "nm -n" or "readelf -s".
  //   runtime address: the address we get from perf data file.
  // The address in perfdata file is runtime address, for pie binaries, runtime
  // address must be translated to binary address (so we can map it to symbols),
  // for non-pie binaries, runtime address always equals to binary address.
  // Parameters:
  //    pid:  process id
  //   addr:  runtime address, as is from perf data
  //    bpi:  binary inforamtion needed to compute the mapping
  uint64_t RuntimeAddressToBinaryAddress(uint64_t pid, uint64_t addr,
                                         const BinaryPerfInfo &bpi) const;

  static const uint64_t kInvalidAddress = static_cast<uint64_t>(-1);

 private:
  // Select mmap events from perfdata file by comparing the mmap event's
  // filename against "match_mmap_name".
  bool SelectMMaps(BinaryPerfInfo *info, const quipper::PerfReader &perf_reader,
                   const quipper::PerfParser &perf_parser,
                   const std::string &match_mmap_name) const;
};

// Utility class that wraps utility functions that need templated
// ELFFile<ELFT> support.
class ELFFileUtilBase {
 protected:
  ELFFileUtilBase() {}

 public:
  virtual ~ELFFileUtilBase() {}

  virtual std::string GetBuildId() = 0;

  // Reads, decodes and then returns the BB address map section of the binary.
  virtual absl::StatusOr<std::vector<llvm::object::BBAddrMap>> GetBbAddrMap(
      const devtools_crosstool_autofdo::BinaryInfo &binary_info) = 0;

  virtual bool ReadLoadableSegments(
      devtools_crosstool_autofdo::BinaryInfo *binary_info) = 0;

 protected:
  static constexpr llvm::StringRef kBuildIDSectionName = ".note.gnu.build-id";
  static constexpr llvm::StringRef kBuildIdNoteName = "GNU";
  static constexpr llvm::StringRef kBbAddrMapSectionName = ".llvm_bb_addr_map";

  friend std::unique_ptr<ELFFileUtilBase> CreateELFFileUtil(
      llvm::object::ObjectFile *object_file);
};

template <class ELFT>
class ELFFileUtil : public ELFFileUtilBase {
 public:
  explicit ELFFileUtil(llvm::object::ObjectFile *object) {
    llvm::object::ELFObjectFile<ELFT> *elf_object =
        llvm::dyn_cast<llvm::object::ELFObjectFile<ELFT>,
                       llvm::object::ObjectFile>(object);
    if (elf_object)
      elf_file_ = &elf_object->getELFFile();
  }

  // Get binary build id.
  std::string GetBuildId() override;

  // Read loadable and executable segment information into BinaryInfo::segments.
  bool ReadLoadableSegments(
      devtools_crosstool_autofdo::BinaryInfo *binary_info) override;

  absl::StatusOr<std::vector<llvm::object::BBAddrMap>> GetBbAddrMap(
      const devtools_crosstool_autofdo::BinaryInfo &binary_info) override;

 private:
  const llvm::object::ELFFile<ELFT> *elf_file_ = nullptr;
};

std::unique_ptr<ELFFileUtilBase> CreateELFFileUtil(
    llvm::object::ObjectFile *object_file);

std::ostream &operator<<(std::ostream &os, const MMapEntry &me);

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_PERFDATA_READER_H_
