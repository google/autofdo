// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Create AutoFDO profile.

#ifndef AUTOFDO_PROFILE_CREATOR_H_
#define AUTOFDO_PROFILE_CREATOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "addr2line.h"
#include "profile_writer.h"
#include "sample_reader.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "third_party/abseil/absl/types/span.h"

namespace devtools_crosstool_autofdo {

class ProfileCreator {
 public:
  explicit ProfileCreator(absl::string_view binary)
      : sample_reader_(nullptr), binary_(binary) {}

  ~ProfileCreator() {
    delete sample_reader_;
  }

  // Returns the total sample counts from a text profile.
  static uint64_t GetTotalCountFromTextProfile(
      absl::string_view input_profile_name);

  // Creates AutoFDO profile, returns true if success, false otherwise.
  bool CreateProfile(const std::string &input_profile_name,
                     const std::string &profiler,
                     devtools_crosstool_autofdo::ProfileWriter *writer,
                     const std::string &output_profile_name,
                     bool store_sym_list_in_profile = false,
                     bool check_lbr_entry = false);

  // Reads samples from the input profile.
  bool ReadSample(absl::string_view input_profile_name,
                  const std::string &profiler);

  // Returns total number of samples collected.
  uint64_t TotalSamples();

  // Returns the SampleReader pointer.
  const devtools_crosstool_autofdo::SampleReader &sample_reader() {
    return *sample_reader_;
  }

  // Computes the profile and updates the given symbol map and addr2line
  // instance.
  bool ComputeProfile(devtools_crosstool_autofdo::SymbolMap *symbol_map,
                      bool check_lbr_entry);

  // Returns true if the sample is from Linux kernel.
  bool IsKernelSample() const {
    assert(sample_reader_);
    return sample_reader_->IsKernelSample();
  }

 private:
  bool ConvertPrefetchHints(const std::string &profile_file,
                            SymbolMap *symbol_map);
  bool CheckAndAssignAddr2Line(SymbolMap *symbol_map, Addr2line *addr2line);

  SampleReader *sample_reader_;
  std::string binary_;
};

// Merges all input_files into output_file. Returns `true` when all merges have
// succeeded. If there are errors reading the output_file (before merge), or
// reading/merging any of the inputs, returns `false` and stops processing
// (i.e. the remaining profiles are not merged).
bool MergeSample(const std::vector<std::string> &input_files,
                 const std::string &profiler, absl::string_view binary,
                 absl::string_view output_file);

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_PROFILE_CREATOR_H_
