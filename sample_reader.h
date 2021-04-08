// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Read the samples from the profile datafile.

#ifndef AUTOFDO_SAMPLE_READER_H_
#define AUTOFDO_SAMPLE_READER_H_

#include <ios>
#include <map>
#include <memory>
#include <regex>  // NOLINT
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/integral_types.h"
#include "base/macros.h"
#include "llvm/Object/ObjectFile.h"
#include "quipper/perf_parser.h"

namespace quipper {
class PerfReader;
}

namespace linux_tools_perf {
union perf_event;
}  // namespace linux_tools_perf

namespace devtools_crosstool_autofdo {

// All counter type is using uint64 instead of int64 because GCC's gcov
// functions only takes unsigned variables.
typedef std::map<uint64, uint64> AddressCountMap;
typedef std::pair<uint64, uint64> Range;
typedef std::map<Range, uint64> RangeCountMap;
typedef std::pair<uint64, uint64> Branch;
typedef std::map<Branch, uint64> BranchCountMap;

// Convert binary data stored in data[...] into text representation.
std::string BinaryDataToAscii(const string &data);

// Reads in the profile data, and represent it in address_count_map_.
class SampleReader {
 public:
  SampleReader() : total_count_(0) {}
  virtual ~SampleReader() {}

  bool ReadAndSetTotalCount();

  const AddressCountMap &address_count_map() const {
    return address_count_map_;
  }

  const RangeCountMap &range_count_map() const {
    return range_count_map_;
  }

  const BranchCountMap &branch_count_map() const {
    return branch_count_map_;
  }

  std::set<uint64> GetSampledAddresses() const;

  // Returns the sample count for a given instruction.
  uint64 GetSampleCountOrZero(uint64 addr) const;
  // Returns the total sampled count.
  uint64 GetTotalSampleCount() const;
  // Returns the max count.
  uint64 GetTotalCount() const {
    return total_count_;
  }
  // Clear all maps to release memory.
  void Clear() {
    address_count_map_.clear();
    range_count_map_.clear();
    branch_count_map_.clear();
  }

 protected:
  // Virtual read function to read from different types of profiles.
  virtual bool Read() = 0;

  uint64 total_count_;
  AddressCountMap address_count_map_;
  RangeCountMap range_count_map_;
  BranchCountMap branch_count_map_;
};

// Base class that reads in the profile from a sample data file.
class FileSampleReader : public SampleReader {
 public:
  explicit FileSampleReader(const std::string &profile_file)
      : profile_file_(profile_file) {}

  virtual bool Append(const std::string &profile_file) = 0;

 protected:
  bool Read() override;

  std::string profile_file_;
};

// Reads/Writes sample data from/to text file.
// The text file format:
//
// number of entries in range_count_map
// from_1-to_1:count_1
// from_2-to_2:count_2
// ......
// from_n-to_n:count_n
// number of entries in address_count_map
// addr_1:count_1
// addr_2:count_2
// ......
// addr_n:count_n
class TextSampleReaderWriter : public FileSampleReader {
 public:
  explicit TextSampleReaderWriter(const std::string &profile_file)
      : FileSampleReader(profile_file) {}
  explicit TextSampleReaderWriter() : FileSampleReader("") { }
  bool Append(const std::string &profile_file) override;
  void Merge(const SampleReader &reader);
  // Writes the profile to file, and appending aux_info at the end.
  bool Write(const char *aux_info);
  bool IsFileExist() const;
  void SetAddressCountMap(const AddressCountMap &map) {
    address_count_map_ = map;
  }
  void IncAddress(uint64 addr) {
    address_count_map_[addr]++;
  }
  void IncRange(uint64 start, uint64 end) {
    range_count_map_[Range(start, end)]++;
  }
  void IncBranch(uint64 from, uint64 to) {
    branch_count_map_[Branch(from, to)]++;
  }
  void set_profile_file(const std::string &file) { profile_file_ = file; }

 private:
  DISALLOW_COPY_AND_ASSIGN(TextSampleReaderWriter);
};

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
  std::unique_ptr<quipper::PerfReader> perf_reader;
  std::unique_ptr<quipper::PerfParser> perf_parser;

  BinaryPerfInfo(const BinaryPerfInfo &) = delete;

  BinaryPerfInfo() {}
  BinaryPerfInfo(BinaryPerfInfo &&bpi)
      : binary_mmaps(std::move(bpi.binary_mmaps)),
        binary_info(std::move(bpi.binary_info)),
        perf_reader(std::move(bpi.perf_reader)),
        perf_parser(std::move(bpi.perf_parser)) {}

  void ResetPerfInfo() {
    perf_parser.reset();
    perf_reader.reset();
    binary_mmaps.clear();
  }

  explicit operator bool() const { return !binary_mmaps.empty(); }
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
  bool SelectPerfInfo(const std::string &perf_file,
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
  bool SelectMMaps(BinaryPerfInfo *info,
                   const std::string &match_mmap_name) const;
};

std::ostream &operator<<(std::ostream &os, const MMapEntry &me);

// Reads in the sample data from 'perf -g' output file.
class PerfDataSampleReader : public FileSampleReader {
 public:
  PerfDataSampleReader(const std::string &profile_file, const std::string &re,
                       const std::string &build_id);
  ~PerfDataSampleReader() override;
  bool Append(const std::string &profile_file) override;

 protected:
  virtual bool MatchBinary(const std::string &name);
  virtual void GetFileNameFromBuildID(const quipper::PerfReader *reader);

  const std::string build_id_;

 private:
  std::set<std::string> focus_bins_;
  const std::regex re_;

  DISALLOW_COPY_AND_ASSIGN(PerfDataSampleReader);
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_SAMPLE_READER_H_
