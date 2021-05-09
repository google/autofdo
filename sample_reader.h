// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Read the samples from the profile datafile.

#ifndef AUTOFDO_SAMPLE_READER_H_
#define AUTOFDO_SAMPLE_READER_H_

#include <cstdint>
#include <map>
#include <regex>  // NOLINT
#include <set>
#include <string>
#include <utility>

#include "base/integral_types.h"
#include "base/macros.h"
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
typedef std::map<uint64_t, uint64_t> AddressCountMap;
typedef std::pair<uint64_t, uint64_t> Range;
typedef std::map<Range, uint64_t> RangeCountMap;
typedef std::pair<uint64_t, uint64_t> Branch;
typedef std::map<Branch, uint64_t> BranchCountMap;

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

  std::set<uint64_t> GetSampledAddresses() const;

  // Returns the sample count for a given instruction.
  uint64_t GetSampleCountOrZero(uint64_t addr) const;
  // Returns the total sampled count.
  uint64_t GetTotalSampleCount() const;
  // Returns the max count.
  uint64_t GetTotalCount() const { return total_count_; }
  // Clear all maps to release memory.
  void Clear() {
    address_count_map_.clear();
    range_count_map_.clear();
    branch_count_map_.clear();
  }

 protected:
  // Virtual read function to read from different types of profiles.
  virtual bool Read() = 0;

  uint64_t total_count_;
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
  void IncAddress(uint64_t addr) { address_count_map_[addr]++; }
  void IncRange(uint64_t start, uint64_t end) {
    range_count_map_[Range(start, end)]++;
  }
  void IncBranch(uint64_t from, uint64_t to) {
    branch_count_map_[Branch(from, to)]++;
  }
  void set_profile_file(const std::string &file) { profile_file_ = file; }

 private:
  DISALLOW_COPY_AND_ASSIGN(TextSampleReaderWriter);
};

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
