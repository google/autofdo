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

// Read the samples from the profile datafile.

#ifndef AUTOFDO_SAMPLE_READER_H_
#define AUTOFDO_SAMPLE_READER_H_

#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>

#include "base/common.h"

namespace linux_tools_perf {
union perf_event;
}  // namespace linux_tools_perf

namespace autofdo {

// All counter type is using uint64 instead of int64 because GCC's gcov
// functions only takes unsigned variables.
typedef map<uint64, uint64> AddressCountMap;
typedef pair<uint64, uint64> Range;
typedef map<Range, uint64> RangeCountMap;
typedef pair<uint64, uint64> Branch;
typedef map<Branch, uint64> BranchCountMap;

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

  set<uint64> GetSampledAddresses() const;

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
  explicit FileSampleReader(const string &profile_file)
      : profile_file_(profile_file) {}

  virtual bool Append(const string &profile_file) = 0;

 protected:
  virtual bool Read();

  string profile_file_;
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
  explicit TextSampleReaderWriter(const string &profile_file) :
      FileSampleReader(profile_file) { }
  explicit TextSampleReaderWriter() : FileSampleReader("") { }
  virtual bool Append(const string &profile_file);
  void Merge(const SampleReader &reader);
  // Writes the profile to file, and appending aux_info at the end.
  bool Write(const char *aux_info);
  bool IsFileExist() const;
  void IncAddress(uint64 addr) {
    address_count_map_[addr]++;
  }
  void IncRange(uint64 start, uint64 end) {
    range_count_map_[Range(start, end)]++;
  }
  void IncBranch(uint64 from, uint64 to) {
    branch_count_map_[Branch(from, to)]++;
  }
  void set_profile_file(const string &file) {
    profile_file_ = file;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TextSampleReaderWriter);
};

// Reads in the sample data from 'perf -g' output file.
class PerfDataSampleReader : public FileSampleReader {
 public:
  explicit PerfDataSampleReader(const string &profile_file,
                                const string &re) :
     FileSampleReader(profile_file), focus_binary_re_(re) { }
  virtual bool Append(const string &profile_file);

 private:
  const string focus_binary_re_;

  DISALLOW_COPY_AND_ASSIGN(PerfDataSampleReader);
};
}  // namespace autofdo

#endif  // AUTOFDO_SAMPLE_READER_H_
