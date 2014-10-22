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

// Create AutoFDO Profile.

#include <memory>

#include "profile_creator.h"

#include "gflags/gflags.h"
#include "base/common.h"
#include "addr2line.h"
#include "gcov.h"
#include "profile.h"
#include "profile_writer.h"
#include "sample_reader.h"
#include "symbol_map.h"
#include "symbolize/elf_reader.h"
#include "module_grouper.h"

namespace autofdo {
uint64 ProfileCreator::GetTotalCountFromTextProfile(
    const string &input_profile_name) {
  ProfileCreator creator("");
  if (!creator.ReadSample(input_profile_name, "text")) {
    return 0;
  }
  return creator.TotalSamples();
}

bool ProfileCreator::CreateProfile(const string &input_profile_name,
                                   const string &profiler,
                                   const string &output_profile_name,
                                   const string &output_format) {
  if (!ReadSample(input_profile_name, profiler)) {
    return false;
  }
  if (!CreateProfileFromSample(output_profile_name, output_format)) {
    return false;
  }
  return true;
}

bool ProfileCreator::ReadSample(const string &input_profile_name,
                                const string &profiler) {
  if (profiler == "perf") {
    // Sets the regular expression to filter samples for a given binary.
    char *dup_name = strdup(binary_.c_str());
    char *strip_ptr = strstr(dup_name, ".unstripped");
    if (strip_ptr) {
      *strip_ptr = 0;
    }
    const char *file_base_name = basename(dup_name);
    CHECK(file_base_name) << "Cannot find basename for: " << binary_;

    ElfReader reader(binary_);

    sample_reader_ = new PerfDataSampleReader(
        input_profile_name, file_base_name);
    free(dup_name);
  } else if (profiler == "text") {
    sample_reader_ = new TextSampleReaderWriter(input_profile_name);
  } else {
    LOG(ERROR) << "Unsupported profiler type: " << profiler;
    return false;
  }
  if (!sample_reader_->ReadAndSetTotalCount()) {
    LOG(ERROR) << "Error reading profile.";
    return false;
  }
  return true;
}

bool ProfileCreator::CreateProfileFromSample(const string &output_profile_name,
                                             const string &output_format) {
  SymbolMap symbol_map(binary_);
  set<uint64> sampled_addrs = sample_reader_->GetSampledAddresses();;
  map<uint64, uint64> sampled_functions =
      symbol_map.GetSampledSymbolStartAddressSizeMap(sampled_addrs);
  Addr2line *addr2line =Addr2line::CreateWithSampledFunctions(
      binary_, &sampled_functions);

  if (addr2line == NULL) {
    LOG(ERROR) << "Error reading binary " << binary_;
    return false;
  }

  Profile profile(sample_reader_, binary_, addr2line, &symbol_map);
  profile.ComputeProfile();

  ModuleGrouper *grouper = ModuleGrouper::GroupModule(
      binary_, GCOV_ELF_SECTION_NAME, &symbol_map);

  ProfileWriter *writer = 0;
  if (output_format == "gcov")
    writer = new AutoFDOProfileWriter(symbol_map, grouper->module_map(),
                                      FLAGS_gcov_version);
  else if (output_format == "llvm")
    writer = new LLVMProfileWriter(symbol_map, grouper->module_map());
  else
    LOG(ERROR) << "Unsupported output profile format: " << output_format;

  bool ret = writer->WriteToFile(output_profile_name);
  delete addr2line;
  delete grouper;
  delete writer;
  return ret;
}

uint64 ProfileCreator::TotalSamples() {
  if (sample_reader_ == NULL) {
    return 0;
  } else {
    return sample_reader_->GetTotalSampleCount();
  }
}

bool MergeSample(const string &input_file, const string &input_profiler,
                 const string &binary, const string &output_file) {
  TextSampleReaderWriter writer(output_file);
  if (writer.IsFileExist()) {
    if (!writer.ReadAndSetTotalCount()) {
      return false;
    }
  }

  ProfileCreator creator(binary);
  if (creator.ReadSample(input_file, input_profiler)) {
    writer.Merge(creator.sample_reader());
    if (writer.Write(NULL)) {
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}
}  // namespace autofdo
