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

#include "sample_reader.h"

#include <string>
#include <utility>

#include "base/logging.h"
#include "quipper/perf_parser.h"

namespace {
// Returns true if name equals full_name, or full_name is empty and name
// matches re.
bool MatchBinary(const string &name, const string &full_name) {
    return full_name == basename(name.c_str());
}
}  // namespace

namespace autofdo {
set<uint64> SampleReader::GetSampledAddresses() const {
  set<uint64> addrs;
  if (range_count_map_.size() > 0) {
    for (const auto &range_count : range_count_map_) {
      addrs.insert(range_count.first.first);
    }
  } else {
    for (const auto &addr_count : address_count_map_) {
      addrs.insert(addr_count.first);
    }
  }
  return addrs;
}

uint64 SampleReader::GetSampleCountOrZero(uint64 addr) const {
  AddressCountMap::const_iterator iter = address_count_map_.find(addr);
  if (iter == address_count_map_.end())
    return 0;
  else
    return iter->second;
}

uint64 SampleReader::GetTotalSampleCount() const {
  uint64 ret = 0;

  if (range_count_map_.size() > 0) {
    for (const auto &range_count : range_count_map_) {
      ret += range_count.second;
    }
  } else {
    for (const auto &addr_count : address_count_map_) {
      ret += addr_count.second;
    }
  }
  return ret;
}

bool SampleReader::ReadAndSetTotalCount() {
  if (!Read()) {
    return false;
  }
  if (range_count_map_.size() > 0) {
    for (const auto &range_count : range_count_map_) {
      total_count_ += range_count.second * (range_count.first.second -
                                            range_count.first.first);
    }
  } else {
    for (const auto &addr_count : address_count_map_) {
      total_count_ += addr_count.second;
    }
  }
  return true;
}

bool FileSampleReader::Read() {
  return Append(profile_file_);
}

bool TextSampleReaderWriter::Append(const string &profile_file) {
  FILE *fp = fopen(profile_file.c_str(), "r");
  if (fp == NULL) {
    LOG(ERROR) << "Cannot open " << profile_file << "to read";
    return false;
  }
  uint64 num_records;

  // Reads in the range_count_map
  if (1 != fscanf(fp, "%llu\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64 from, to, count;
    if (3 != fscanf(fp, "%llx-%llx:%llu\n", &from, &to, &count)) {
      LOG(ERROR) << "Error reading from " << profile_file;
      fclose(fp);
      return false;
    }
    range_count_map_[Range(from, to)] += count;
  }

  // Reads in the addr_count_map
  if (1 != fscanf(fp, "%llu\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64 addr, count;
    if (2 != fscanf(fp, "%llx:%llu\n", &addr, &count)) {
      LOG(ERROR) << "Error reading from " << profile_file;
      fclose(fp);
      return false;
    }
    address_count_map_[addr] += count;
  }

  // Reads in the branch_count_map
  if (1 != fscanf(fp, "%llu\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64 from, to, count;
    if (3 != fscanf(fp, "%llx->%llx:%llu\n", &from, &to, &count)) {
      LOG(ERROR) << "Error reading from " << profile_file;
      fclose(fp);
      return false;
    }
    branch_count_map_[Branch(from, to)] += count;
  }
  fclose(fp);
  return true;
}

void TextSampleReaderWriter::Merge(const SampleReader &reader) {
  for (const auto &range_count : reader.range_count_map()) {
    range_count_map_[range_count.first] += range_count.second;
  }
  for (const auto &addr_count : reader.address_count_map()) {
    address_count_map_[addr_count.first] += addr_count.second;
  }
  for (const auto &branch_count : reader.branch_count_map()) {
    branch_count_map_[branch_count.first] += branch_count.second;
  }
}

bool TextSampleReaderWriter::Write(const char *aux_info) {
  FILE *fp = fopen(profile_file_.c_str(), "w");
  if (fp == NULL) {
    LOG(ERROR) << "Cannot open " << profile_file_ << " to write";
    return false;
  }

  fprintf(fp, "%" PRIuS "\n", range_count_map_.size());
  for (const auto &range_count : range_count_map_) {
    fprintf(fp, "%llx-%llx:%llu\n", range_count.first.first,
            range_count.first.second, range_count.second);
  }
  fprintf(fp, "%" PRIuS "\n", address_count_map_.size());
  for (const auto &addr_count : address_count_map_) {
    fprintf(fp, "%llx:%llu\n", addr_count.first, addr_count.second);
  }
  fprintf(fp, "%" PRIuS "\n", branch_count_map_.size());
  for (const auto &branch_count : branch_count_map_) {
    fprintf(fp, "%llx->%llx:%llu\n", branch_count.first.first,
            branch_count.first.second, branch_count.second);
  }
  if (aux_info) {
    fprintf(fp, "%s", aux_info);
  }
  fclose(fp);
  return true;
}

bool TextSampleReaderWriter::IsFileExist() const {
  FILE *fp = fopen(profile_file_.c_str(), "r");
  if (fp == NULL) {
    return false;
  } else {
    fclose(fp);
    return true;
  }
}

bool PerfDataSampleReader::Append(const string &profile_file) {
  quipper::PerfParser parser;
  if (!parser.ReadFile(profile_file) || !parser.ParseRawEvents()) {
    return false;
  }

  string focus_binary = focus_binary_re_;

  // If we can find build_id from binary, and the exact build_id was found
  // in the profile, then we use focus_binary to match samples. Otherwise,
  // focus_binary_re_ is used to match the binary name with the samples.
  for (const auto &event : parser.parsed_events()) {
    if (!event.raw_event ||
        event.raw_event->header.type != PERF_RECORD_SAMPLE) {
      continue;
    }
    if (MatchBinary(event.dso_and_offset.dso_name(), focus_binary)) {
      address_count_map_[event.dso_and_offset.offset()]++;
    }
    if (event.branch_stack.size() > 0 &&
        MatchBinary(event.branch_stack[0].to.dso_name(), focus_binary) &&
        MatchBinary(event.branch_stack[0].from.dso_name(), focus_binary)) {
      branch_count_map_[Branch(event.branch_stack[0].from.offset(),
                               event.branch_stack[0].to.offset())]++;
    }
    for (int i = 1; i < event.branch_stack.size(); i++) {
      if (!MatchBinary(event.branch_stack[i].to.dso_name(), focus_binary)) {
        continue;
      }
      uint64 begin = event.branch_stack[i].to.offset();
      uint64 end = event.branch_stack[i - 1].from.offset();
      // The interval between two taken branches should not be too large.
      if (end < begin || end - begin > (1 << 20)) {
        LOG(WARNING) << "Bogus LBR data: " << begin << "->" << end;
        continue;
      }
      range_count_map_[Range(begin, end)]++;
      if (MatchBinary(event.branch_stack[i].from.dso_name(),
                      focus_binary)) {
        branch_count_map_[Branch(event.branch_stack[i].from.offset(),
                                 event.branch_stack[i].to.offset())]++;
      }
    }
  }
  return true;
}
}  // namespace autofdo
