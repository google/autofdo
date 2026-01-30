// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Write profile to afdo file.

#include "profile_writer.h"

#include <stdio.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "gcov.h"
#include "profile.h"
#include "source_info.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/str_format.h"

// sizeof(gcov_unsigned_t)
#define SIZEOF_UNSIGNED 4

ABSL_FLAG(bool, debug_dump, false,
            "If set, emit additional debugging dumps to stderr.");

namespace devtools_crosstool_autofdo {
// Opens the output file, and writes the header.
bool AutoFDOProfileWriter::WriteHeader(const std::string &output_filename) {
  if (gcov_open(output_filename.c_str(), -1) == -1) {
    LOG(FATAL) << "Cannot open file " << output_filename;
    return false;
  }

  gcov_write_unsigned(GCOV_DATA_MAGIC);
  gcov_write_unsigned(gcov_version_);
  gcov_write_unsigned(0);
  return true;
}

// Finishes writing, closes the output file.
bool AutoFDOProfileWriter::WriteFinish() {
  if (gcov_close()) {
    LOG(ERROR) << "Cannot close the gcov file.";
    return false;
  }
  return true;
}

class SourceProfileLengther: public SymbolTraverser {
 public:
  explicit SourceProfileLengther(const SymbolMap &symbol_map)
      : length_(0), num_functions_(0) {
    Start(symbol_map);
  }

  // This type is neither copyable nor movable.
  SourceProfileLengther(const SourceProfileLengther &) = delete;
  SourceProfileLengther &operator=(const SourceProfileLengther &) = delete;

  int length() {return length_ + num_functions_ * 2;}
  int num_functions() {return num_functions_;}

 protected:
  virtual void VisitTopSymbol(const std::string &name, const Symbol *node) {
    num_functions_++;
  }

  virtual void Visit(const Symbol *node) {
    // func_name, num_pos_counts, num_callsites
    length_ += 3;
    // offset_discr, num_targets, count * 2
    length_ += node->pos_counts.size() * 4;
    // offset_discr
    length_ += node->callsites.size();
    for (const auto &pos_count : node->pos_counts) {
      // type, func_name * 2, count * 2
      length_ += pos_count.second.target_map.size() * 5;
    }
  }

 private:
  int length_;
  int num_functions_;
};

class SourceProfileWriter: public SymbolTraverser {
 public:
  // This type is neither copyable nor movable.
  SourceProfileWriter(const SourceProfileWriter &) = delete;
  SourceProfileWriter &operator=(const SourceProfileWriter &) = delete;

  static void Write(const SymbolMap &symbol_map, const StringIndexMap &map) {
    SourceProfileWriter writer(map);
    writer.Start(symbol_map);
  }

 protected:
  virtual void Visit(const Symbol *node) {
    gcov_write_unsigned(node->pos_counts.size());
    gcov_write_unsigned(node->callsites.size());
    for (const auto &pos_count : node->pos_counts) {
      uint64_t value = pos_count.first;
      gcov_write_unsigned(SourceInfo::GenerateCompressedOffset(value));
      gcov_write_unsigned(pos_count.second.target_map.size());
      gcov_write_counter(pos_count.second.count);
      TargetCountPairs target_counts;
      GetSortedTargetCountPairs(pos_count.second.target_map, &target_counts);
      for (const auto &target_count : pos_count.second.target_map) {
        gcov_write_unsigned(HIST_TYPE_INDIR_CALL_TOPN);
        gcov_write_counter(GetStringIndex(target_count.first));
        gcov_write_counter(target_count.second);
      }
    }
  }

  virtual void VisitTopSymbol(const std::string &name, const Symbol *node) {
    gcov_write_counter(node->head_count);
    gcov_write_unsigned(GetStringIndex(Symbol::Name(name.c_str())));
  }

  virtual void VisitCallsite(const Callsite &callsite) {
    uint64_t value = callsite.location;
    gcov_write_unsigned(SourceInfo::GenerateCompressedOffset(value));
    gcov_write_unsigned(GetStringIndex(Symbol::Name(callsite.callee_name)));
  }

 private:
  explicit SourceProfileWriter(const StringIndexMap &map) : map_(map) {}

  int GetStringIndex(const std::string &str) {
    StringIndexMap::const_iterator ret = map_.find(str);
    CHECK(ret != map_.end());
    return ret->second;
  }

  const StringIndexMap &map_;
};

void AutoFDOProfileWriter::WriteFunctionProfile() {
  typedef std::map<std::string, int> StringIndexMap;
  // Map from a string to its index in this map. Providing a partial
  // ordering of all output strings.
  StringIndexMap string_index_map;
  int length_4bytes = 0, current_name_index = 0;
  string_index_map[std::string()] = 0;

  FileIndexMap file_map;
  StringTableUpdater::Update(*symbol_map_, &string_index_map, &file_map);

  // Write out the GCOV_TAG_AFDO_SUMMARY section.
  if (absl::GetFlag(FLAGS_gcov_version) >= 3) {
    ProfileSummaryInformation info = ProfileSummaryComputer::Compute(
        *symbol_map_, {std::begin(ProfileSummaryInformation::default_cutoffs),
                       std::end(ProfileSummaryInformation::default_cutoffs)});
    gcov_write_unsigned(GCOV_TAG_AFDO_SUMMARY);
    gcov_write_counter(info.total_count_);
    gcov_write_counter(info.max_count_);
    gcov_write_counter(info.max_function_count_);
    gcov_write_counter(info.num_counts_);
    gcov_write_counter(info.num_functions_);
    gcov_write_counter(info.detailed_summaries_.size());
    for (const auto &detailed_summary : info.detailed_summaries_) {
      gcov_write_unsigned(detailed_summary.cutoff_);
      gcov_write_counter(detailed_summary.min_count_);
      gcov_write_counter(detailed_summary.num_counts_);
    }
  }

  for (auto &name_index : string_index_map) {
    name_index.second = current_name_index++;
    length_4bytes += (name_index.first.size()
                      + SIZEOF_UNSIGNED) / SIZEOF_UNSIGNED;
    length_4bytes += 1;
  }
  length_4bytes += 1;

  // Writes the GCOV_TAG_AFDO_FILE_NAMES section.
  gcov_write_unsigned(GCOV_TAG_AFDO_FILE_NAMES);
  gcov_write_unsigned(length_4bytes);
  // File names in the profile are a feature of GCOV version 3.
  if (absl::GetFlag(FLAGS_gcov_version) >= 3) {
    gcov_write_unsigned(file_map.Size());
    for (const auto &file_name : file_map.GetFileNames()) {
      gcov_write_string(file_name.c_str());
    }
  }
  gcov_write_unsigned(string_index_map.size());
  for (const auto &[name, index] : string_index_map) {
    char *c = strdup(name.c_str());
    int len = strlen(c);
    // Workaround https://gcc.gnu.org/bugzilla/show_bug.cgi?id=64346
    // We should not have D4Ev in our profile because it does not exist
    // in symbol table and would lead to undefined symbols during linking.
    if (len > 5 &&
        (!strcmp(c + len - 4, "D4Ev") || !strcmp(c + len - 4, "C4Ev"))) {
      c[len - 3] = '2';
    } else if (len > 7 && !strcmp(c + len - 6, "C4EPKc")) {
      c[len - 5] = '2';
    } else if (len > 12 && !strcmp(c + len - 11, "C4EPKcRKS2_")) {
      c[len - 10] = '2';
    }
    gcov_write_string(c);
    if (absl::GetFlag(FLAGS_gcov_version) >= 3) {
      if (int lookup = file_map.GetFileIndex(name); lookup != -1) {
        gcov_write_unsigned(lookup);
      } else {
        gcov_write_unsigned(-1);
      }
    }

    free(c);
  }

  // Compute the length of the GCOV_TAG_AFDO_FUNCTION section.
  SourceProfileLengther length(*symbol_map_);
  gcov_write_unsigned(GCOV_TAG_AFDO_FUNCTION);
  gcov_write_unsigned(length.length() + 1);
  gcov_write_unsigned(length.num_functions());
  SourceProfileWriter::Write(*symbol_map_, string_index_map);
}

void AutoFDOProfileWriter::WriteModuleGroup() {
  gcov_write_unsigned(GCOV_TAG_MODULE_GROUPING);
  // Length of the section
  gcov_write_unsigned(0);
  // Number of modules
  gcov_write_unsigned(0);
}

void AutoFDOProfileWriter::WriteWorkingSet() {
  gcov_write_unsigned(GCOV_TAG_AFDO_WORKING_SET);
  gcov_write_unsigned(3 * NUM_GCOV_WORKING_SETS);
  const gcov_working_set_info *working_set = symbol_map_->GetWorkingSets();
  for (int i = 0; i < NUM_GCOV_WORKING_SETS; i++) {
    gcov_write_unsigned(working_set[i].num_counters / WORKING_SET_INSN_PER_BB);
    gcov_write_counter(working_set[i].min_counter);
  }
}

bool AutoFDOProfileWriter::WriteToFile(const std::string &output_filename) {
  if (absl::GetFlag(FLAGS_debug_dump))
    Dump();

  if (!WriteHeader(output_filename)) {
    return false;
  }
  WriteFunctionProfile();
  WriteModuleGroup();
  WriteWorkingSet();
  if (!WriteFinish()) {
    return false;
  }
  return true;
}

bool ProfileSummaryInformation::operator==(
    const ProfileSummaryInformation &other) const {
  return total_count_ == other.total_count_
      && max_count_ == other.max_count_
      && max_function_count_ == other.max_function_count_
      && num_counts_ == other.num_counts_
      && num_functions_ == other.num_functions_
      && std::equal(detailed_summaries_.begin(), detailed_summaries_.end(),
                    other.detailed_summaries_.begin());
}

bool ProfileSummaryInformation::DetailedSummary::operator==(
    const ProfileSummaryInformation::DetailedSummary &other) const {
  return cutoff_ == other.cutoff_
      && min_count_ == other.min_count_
      && num_counts_ == other.num_counts_;
}

ProfileSummaryComputer::ProfileSummaryComputer()
    : cutoffs_{std::begin(ProfileSummaryInformation::default_cutoffs),
               std::end(ProfileSummaryInformation::default_cutoffs)} {}

ProfileSummaryComputer::ProfileSummaryComputer(std::vector<uint32_t> cutoffs)
    : cutoffs_{std::move(cutoffs)} {}

void ProfileSummaryComputer::VisitTopSymbol(const std::string &name,
                                            const Symbol *node) {
  info_.num_functions_++;
  info_.max_function_count_ =
      std::max(info_.max_function_count_, node->head_count);
}

void ProfileSummaryComputer::Visit(const Symbol *node) {
  // There is a slight difference against the values computed by
  // SampleProfileSummaryBuilder/LLVMProfileBuilder as it represents
  // lineno:discriminator pairs as 16:32 bits. This causes line numbers >=
  // UINT16_MAX to be counted incorrectly (see GetLineNumberFromOffset in
  // source_info.h) as they collide with line numbers < UINT16_MAX. This issue
  // is completely avoided here by just not using the offset info at all.
  for (auto &pos_count : node->pos_counts) {
    uint64_t count = pos_count.second.count;
    info_.total_count_ += count;
    info_.max_count_ = std::max(info_.max_count_, count);
    info_.num_counts_++;
    count_frequencies_[count]++;
  }
}

void ProfileSummaryComputer::ComputeDetailedSummary() {
  auto iter = count_frequencies_.begin();
  auto end = count_frequencies_.end();

  uint32_t counts_seen = 0;
  uint64_t curr_sum = 0;
  uint64_t count = 0;

  for (const uint32_t cutoff : cutoffs_) {
    assert(cutoff <= 999'999);
    constexpr int scale = 1'000'000;
    using uint128_t = unsigned __int128;
    uint128_t desired_count = info_.total_count_;
    desired_count = desired_count * uint128_t(cutoff);
    desired_count = desired_count / uint128_t(scale);
    // This should never fail as cutoff is always <= scale, so
    // (info_.total_count_ * (cutoff / scale)) is always <= info_.total_count_.
    assert(desired_count <= info_.total_count_);
    while (curr_sum < desired_count && iter != end) {
      count = iter->first;
      uint32_t freq = iter->second;
      curr_sum += (count * freq);
      counts_seen += freq;
      iter++;
    }
    // curr_sum is the cumulative sum of frequencies, of which
    // info_.total_count_ is the maximum value (as computed in
    // ProfileSummaryComputer::Visit). Thus, this assertion will only fail if
    // desired_count > info_.total_count_ and the maximum value that curr_sum
    // can sum to is lesser than it.
    assert(curr_sum >= desired_count);
    info_.detailed_summaries_.push_back({cutoff, count, counts_seen});
  }
}

// Debugging support.  ProfileDumper emits a detailed dump of the contents
// of the input profile.
class ProfileDumper : public SymbolTraverser {
 public:
  // This type is neither copyable nor movable.
  ProfileDumper(const ProfileDumper &) = delete;
  ProfileDumper &operator=(const ProfileDumper &) = delete;

  static void Write(const SymbolMap &symbol_map, const StringIndexMap &map) {
    ProfileDumper writer(map);
    writer.Start(symbol_map);
  }

 protected:
  void DumpSourceInfo(SourceInfo info, int indent) {
    printf("%*sDirectory name: %s\n", indent, " ", info.dir_name.c_str());
    printf("%*sFile name:      %s\n", indent, " ",
           info.file_name.c_str());
    printf("%*sFunction name:  %s\n", indent, " ", info.func_name);
    printf("%*sStart line:     %u\n", indent, " ", info.start_line);
    printf("%*sLine:           %u\n", indent, " ", info.line);
    printf("%*sDiscriminator:  %u\n", indent, " ", info.discriminator);
  }

  void PrintSourceLocation(uint32_t start_line, uint64_t offset) {
    uint32_t line = SourceInfo::GetLineNumberFromOffset(offset);
    uint32_t discriminator = SourceInfo::GetDiscriminatorFromOffset(offset);
    if (discriminator) {
      printf("%u.%u: ", line + start_line, discriminator);
    } else {
      printf("%u: ", line + start_line);
    }
  }

  virtual void Visit(const Symbol *node) {
    printf("Writing symbol: ");
    node->Dump(4);
    printf("\n");
    printf("Source information:\n");
    DumpSourceInfo(node->info, 0);
    printf("\n");
    absl::PrintF("Total sampled count:            %u\n",
                 static_cast<uint64_t>(node->total_count));
    absl::PrintF("Total sampled count in head bb: %u\n",
                 static_cast<uint64_t>(node->head_count));
    printf("\n");
    printf("Call sites:\n");
    int i = 0;
    for (const auto &[site, symbol] : node->callsites) {
      printf("  #%d: site\n", i);
      printf("    location: %lu\n", site.location);
      printf("    callee_name: %s\n", site.callee_name);
      printf("  #%d: symbol: ", i);
      symbol->Dump(0);
      printf("\n");
      i++;
    }

    absl::PrintF("node->pos_counts.size() = %u\n",
                 static_cast<uint64_t>(node->pos_counts.size()));
    absl::PrintF("node->callsites.size() = %u\n",
                 static_cast<uint64_t>(node->callsites.size()));
    std::vector<uint64_t> positions;
    for (const auto &[pos, count] : node->pos_counts)
      positions.push_back(pos);
    std::sort(positions.begin(), positions.end());
    i = 0;
    for (const auto &pos : positions) {
      PositionCountMap::const_iterator pos_count = node->pos_counts.find(pos);
      DCHECK(pos_count != node->pos_counts.end());
      uint64_t location = pos_count->first;
      ProfileInfo info = pos_count->second;

      printf("#%d: location (line[.discriminator]) = ", i);
      PrintSourceLocation(node->info.start_line, location);
      printf("\n");
      absl::PrintF("#%d: profile info execution count = %u\n", i, info.count);
      absl::PrintF("#%d: profile info number of instructions = %u\n", i,
                   info.num_inst);
      TargetCountPairs target_counts;
      GetSortedTargetCountPairs(info.target_map, &target_counts);
      absl::PrintF("#%d: profile info target map size = %u\n", i,
                   static_cast<uint64_t>(info.target_map.size()));
      printf("#%d: info.target_map:\n", i);
      for (const auto &[target, count] : info.target_map) {
        printf("\tGetStringIndex(target_count.first): %d\n",
               GetStringIndex(target));
        absl::PrintF("\ttarget_count.second: %u\n", count);
      }
      printf("\n");
      i++;
    }
  }

  virtual void VisitTopSymbol(const std::string &name, const Symbol *node) {
    printf("VisitTopSymbol: %s\n", name.c_str());
    node->Dump(0);
    absl::PrintF("node->head_count: %u\n", node->head_count);
    printf("GetStringIndex(%s): %u\n", name.c_str(), GetStringIndex(name));
    printf("\n");
  }

  virtual void VisitCallsite(const Callsite &callsite) {
    printf("VisitCallSite: %s\n", callsite.callee_name);
    printf("callsite.location: %lu\n", callsite.location);
    printf("GetStringIndex(callsite.callee_name): %u\n",
           GetStringIndex(callsite.callee_name ? callsite.callee_name
                                               : std::string()));
  }

 private:
  explicit ProfileDumper(const StringIndexMap &map) : map_(map) {}

  int GetStringIndex(const std::string &str) {
    StringIndexMap::const_iterator ret = map_.find(str);
    CHECK(ret != map_.end());
    return ret->second;
  }

  const StringIndexMap &map_;
};

// Emit a dump of the input profile on stdout.
void ProfileWriter::Dump() {
  StringIndexMap string_index_map;
  FileIndexMap file_map;
  StringTableUpdater::Update(*symbol_map_, &string_index_map, &file_map);
  SourceProfileLengther length(*symbol_map_);
  printf("Length of symbol map: %d\n", length.length() + 1);
  printf("Number of functions:  %d\n", length.num_functions());
  ProfileDumper::Write(*symbol_map_, string_index_map);
}

}  // namespace devtools_crosstool_autofdo
