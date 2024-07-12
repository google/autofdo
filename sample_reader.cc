// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Read the samples from the profile datafile.

#include "sample_reader.h"

#include <inttypes.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ios>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/port.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "quipper/perf_parser.h"
#include "quipper/perf_reader.h"

ABSL_FLAG(uint64_t, strip_dup_backedge_stride_limit, 0x1000,
          "Controls the limit of backedge stride hold by the heuristic "
          "to strip duplicated entries in LBR stack. ");

namespace devtools_crosstool_autofdo {

PerfDataSampleReader::PerfDataSampleReader(absl::string_view profile_file,
                                           const std::string &re,
                                           absl::string_view build_id)
    : FileSampleReader(profile_file), build_id_(build_id), re_(re.c_str()) {}

PerfDataSampleReader::~PerfDataSampleReader() {}

// Returns true if the DSO name of dso_and_offset equals any binary path in
// focus_bins_, or focus_bins_ is empty and name matches re_.
bool PerfDataSampleReader::MatchBinary(
    const quipper::ParsedEvent::DSOAndOffset &dso_and_offset) {
  // If we already know whether we've accepted the given dso before, return
  // the cached result.
  auto match_cache_it = match_cache_.find(dso_and_offset.dso_info_);
  if (match_cache_it != match_cache_.end()) {
    return match_cache_it->second;
  }
  bool is_found = false;
  if (focus_bins_.empty()) {
    is_found = std::regex_search(dso_and_offset.dso_name().c_str(), re_);
  } else {
    std::string name = dso_and_offset.dso_name();
    if (!name.empty()) {
      for (const auto &binary_path : focus_bins_) {
        if (name == binary_path) {
          is_found = true;
          break;
        }
      }
    }
  }
  match_cache_[dso_and_offset.dso_info_] = is_found;
  return is_found;
}

// Stores matching binary paths to focus_bins_ for a given build_id_.
void PerfDataSampleReader::GetFileNameFromBuildID(
    const quipper::PerfReader *reader) {
  std::map<std::string, std::string> name_buildid_map;
  reader->GetFilenamesToBuildIDs(&name_buildid_map);

  focus_bins_.clear();
  for (const auto &name_buildid : name_buildid_map) {
    if (name_buildid.second == build_id_) {
      // The build ID event reports the kernel name as '[kernel.kallsyms]'.
      // However, the MMAP event reports the kernel image as either
      // '[kernel.kallsyms]_stext' or '[kernel.kallsyms]_text'.
      // Additionally if the trace is recorded (or decoded) with --vmlinux
      // the name in the build ID event will be a full path to the vmlinux file.
      // If a build_id is associated with the kernel image name, include the
      // alternate names in focus_bins_ too.
      const size_t vmlinux_len = std::strlen("vmlinux");
      if (name_buildid.first == "[kernel.kallsyms]" ||
          (name_buildid.first.size() >= vmlinux_len &&
           !name_buildid.first.compare(name_buildid.first.size() - vmlinux_len,
                                       vmlinux_len, "vmlinux"))) {
        focus_bins_.insert("[kernel.kallsyms]");
        focus_bins_.insert("[kernel.kallsyms]_stext");
        focus_bins_.insert("[kernel.kallsyms]_text");
        is_kernel_ = true;
      } else {
        focus_bins_.insert(name_buildid.first);
      }
    }
  }
  if (focus_bins_.empty()) {
    LOG(ERROR) << "Cannot find binary with buildid " << build_id_;
  }
}

std::set<uint64_t> SampleReader::GetSampledAddresses() const {
  std::set<uint64_t> addrs;
  if (!range_count_map_.empty()) {
    for (const auto &[range, count] : range_count_map_) {
      addrs.insert(range.first);
    }
  } else {
    for (const auto &[addr, count] : address_count_map_) {
      addrs.insert(addr);
    }
  }
  return addrs;
}

uint64_t SampleReader::GetSampleCountOrZero(uint64_t addr) const {
  AddressCountMap::const_iterator iter = address_count_map_.find(addr);
  if (iter == address_count_map_.end())
    return 0;
  else
    return iter->second;
}

uint64_t SampleReader::GetTotalSampleCount() const {
  uint64_t ret = 0;

  if (!range_count_map_.empty()) {
    for (const auto &[range, count] : range_count_map_) {
      ret += count;
    }
  } else {
    for (const auto &[addr, count] : address_count_map_) {
      ret += count;
    }
  }
  return ret;
}

bool SampleReader::ReadAndSetTotalCount() {
  if (!Read()) {
    return false;
  }
  if (!range_count_map_.empty()) {
    for (const auto &[range, count] : range_count_map_) {
      total_count_ += count * (1 + range.second - range.first);
    }
  } else {
    for (const auto &[addr, count] : address_count_map_) {
      total_count_ += count;
    }
  }
  return true;
}

bool FileSampleReader::Read() { return Append(profile_file_); }

bool TextSampleReaderWriter::Append(const std::string &profile_file) {
  FILE *fp = fopen(profile_file.c_str(), "r");
  if (fp == nullptr) {
    LOG(ERROR) << "Cannot open " << profile_file << " to read";
    return false;
  }
  uint64_t num_records;

  // Reads in the range_count_map
  if (1 != fscanf(fp, "%" SCNu64 "\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64_t from, to, count;
    if (3 != fscanf(fp, "%" SCNx64 "-%" SCNx64 ":%" SCNu64 "\n", &from, &to,
                    &count)) {
      LOG(ERROR) << "Error reading from " << profile_file;
      fclose(fp);
      return false;
    }
    range_count_map_[Range(from, to)] += count;
  }

  // Reads in the addr_count_map
  if (1 != fscanf(fp, "%" SCNu64 "\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64_t addr, count;
    if (2 != fscanf(fp, "%" SCNx64 ":%" SCNu64 "\n", &addr, &count)) {
      LOG(ERROR) << "Error reading from " << profile_file;
      fclose(fp);
      return false;
    }
    address_count_map_[addr] += count;
  }

  // Reads in the branch_count_map
  if (1 != fscanf(fp, "%" SCNu64 "\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64_t from, to, count;
    if (3 != fscanf(fp, "%" SCNx64 "->%" SCNx64 ":%" SCNu64 "\n", &from, &to,
                    &count)) {
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
  for (const auto &[range, count] : reader.range_count_map()) {
    range_count_map_[range] += count;
  }
  for (const auto &[addr, count] : reader.address_count_map()) {
    address_count_map_[addr] += count;
  }
  for (const auto &[branch, count] : reader.branch_count_map()) {
    branch_count_map_[branch] += count;
  }
}

bool TextSampleReaderWriter::Write(const char *aux_info) {
  FILE *fp = fopen(profile_file_.c_str(), "w");
  if (fp == nullptr) {
    LOG(ERROR) << "Cannot open " << profile_file_ << " to write";
    return false;
  }

  fprintf(fp, "%" PRIuS "\n", range_count_map_.size());
  for (const auto &[range, count] : range_count_map_) {
    absl::FPrintF(fp, "%x-%x:%u\n", range.first, range.second, count);
  }
  fprintf(fp, "%" PRIuS "\n", address_count_map_.size());
  for (const auto &[addr, count] : address_count_map_) {
    absl::FPrintF(fp, "%x:%u\n", addr, count);
  }
  fprintf(fp, "%" PRIuS "\n", branch_count_map_.size());
  for (const auto &[branch, count] : branch_count_map_) {
    absl::FPrintF(fp, "%x->%x:%u\n", branch.first, branch.second, count);
  }
  if (aux_info) {
    fprintf(fp, "%s", aux_info);
  }
  fclose(fp);
  return true;
}

bool TextSampleReaderWriter::IsFileExist() const {
  FILE *fp = fopen(profile_file_.c_str(), "r");
  if (fp == nullptr) {
    return false;
  } else {
    fclose(fp);
    return true;
  }
}

bool PerfDataSampleReader::Append(const std::string &profile_file) {
  quipper::PerfReader reader;
  quipper::PerfParser parser(&reader);
  if (!reader.ReadFile(profile_file) || !parser.ParseRawEvents()) {
    return false;
  }

  // If we can find build_id from binary, and the exact build_id was found
  // in the profile, then we use focus_bins to match samples. Otherwise,
  // focus_binary_re_ is used to match the binary name with the samples.
  if (!build_id_.empty()) {
    GetFileNameFromBuildID(&reader);
    if (focus_bins_.empty()) {
      // The binary with build_id_ was not found in perf.data.
      // That could happen when e.g. perf.data is a system-wide profile,
      // and the binary of interest was not running when the profile
      // was collected.
      return true;
    }
  } else {
    LOG(INFO) << "No buildid found in binary";
  }

  for (const auto &event : parser.parsed_events()) {
    if (!event.event_ptr ||
        event.event_ptr->header().type() != quipper::PERF_RECORD_SAMPLE) {
      continue;
    }
    if (MatchBinary(event.dso_and_offset)) {
      address_count_map_[event.dso_and_offset.offset()]++;
    }
    int start_index = 0;
    while (start_index < event.branch_stack.size() &&
           event.branch_stack[start_index].spec ==
               quipper::PERF_BR_SPEC_WRONG_PATH) {
      start_index++;
    }
    if (event.branch_stack.size() > start_index &&
        MatchBinary(event.branch_stack[start_index].to) &&
        MatchBinary(event.branch_stack[start_index].from)) {
      branch_count_map_[Branch(event.branch_stack[start_index].from.offset(),
                               event.branch_stack[start_index].to.offset())]++;
    }
    for (int i = start_index + 1; i < event.branch_stack.size(); i++) {
      if (event.branch_stack[i].spec == quipper::PERF_BR_SPEC_WRONG_PATH) {
        continue;
      }

      if (!MatchBinary(event.branch_stack[i].to)) {
        continue;
      }

      // if from_ip map in perf_data_converter PerfParser::MapBranchStack fails but to_ip map succeeeds.
      // Then from.offset() == 0 and to.offset() == ${offset}.
      // This will cause the following check to erroneously warning "Bogus LBR data".
      if (event.branch_stack[i - 1].from.dso_info_ == NULL) {
        continue;
      }

      // TODO(b/62827958): Get rid of this temporary workaround once the issue
      // of duplicate entries in LBR is resolved. It only happens at the head
      // of the LBR. In addition, it is possible that the duplication is
      // legitimate if there's self loop. However, it's very rare to have basic
      // blocks larger than 0x1000 formed by such self loop. So, we're ignoring
      // duplication when the resulting basic block is larger than 0x1000 (the
      // default value of FLAGS_strip_dup_backedge_stride_limit).
      if (i == 1 &&
          (event.branch_stack[0].from.offset() ==
           event.branch_stack[1].from.offset()) &&
          (event.branch_stack[0].to.offset() ==
           event.branch_stack[1].to.offset()) &&
          (event.branch_stack[0].from.offset() -
               event.branch_stack[0].to.offset() >
           absl::GetFlag(FLAGS_strip_dup_backedge_stride_limit))) {
        if (event.branch_stack[1].from.dso_info_ == NULL)
          LOG(WARNING) << "Bogus LBR data (duplicated top entry)";
        continue;
      }
      uint64_t begin = event.branch_stack[i].to.offset();
      uint64_t end = event.branch_stack[i - 1].from.offset();
      // The interval between two taken branches should not be too large.
      if (end < begin || end - begin > (1 << 20)) {
        const std::string reason =
            (end < begin ? "(range is negative)" : "(range is too large)");
        LOG(WARNING) << "Bogus LBR data " << reason << ": " << std::hex << begin
                     << "->" << end << " index=" << i;
        continue;
      }
      range_count_map_[Range(begin, end)]++;
      if (MatchBinary(event.branch_stack[i].from)) {
        branch_count_map_[Branch(event.branch_stack[i].from.offset(),
                                 event.branch_stack[i].to.offset())]++;
      }
    }
  }
  return true;
}
}  // namespace devtools_crosstool_autofdo
