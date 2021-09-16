// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Create AutoFDO profile.

#ifndef AUTOFDO_PROFILE_CREATOR_H_
#define AUTOFDO_PROFILE_CREATOR_H_

#include <cstdint>

#include "addr2line.h"
#include "profile_writer.h"
#include "sample_reader.h"
#include "symbol_map.h"

namespace devtools_crosstool_autofdo {

class ProfileCreator {
 public:
  explicit ProfileCreator(const std::string &binary)
      : sample_reader_(nullptr), binary_(binary) {}

  ~ProfileCreator() {
    delete sample_reader_;
  }

  // Returns the total sample counts from a text profile.
  static uint64_t GetTotalCountFromTextProfile(
      const std::string &input_profile_name);

  // Creates AutoFDO profile, returns true if success, false otherwise.
  bool CreateProfile(const std::string &input_profile_name,
                     const std::string &profiler,
                     devtools_crosstool_autofdo::ProfileWriter *writer,
                     const std::string &output_profile_name,
                     bool store_sym_list_in_profile = false);

  // Reads samples from the input profile.
  bool ReadSample(const std::string &input_profile_name,
                  const std::string &profiler);

  // Returns total number of samples collected.
  uint64_t TotalSamples();

  // Returns the SampleReader pointer.
  const devtools_crosstool_autofdo::SampleReader &sample_reader() {
    return *sample_reader_;
  }

  // Computes the profile and updates the given symbol map and addr2line
  // instance.
  bool ComputeProfile(devtools_crosstool_autofdo::SymbolMap *symbol_map);

 private:
  bool ConvertPrefetchHints(const std::string &profile_file,
                            SymbolMap *symbol_map);
  bool CheckAndAssignAddr2Line(SymbolMap *symbol_map, Addr2line *addr2line);

  SampleReader *sample_reader_;
  std::string binary_;
};

bool MergeSample(const std::string &input_file,
                 const std::string &input_profiler, const std::string &binary,
                 const std::string &output_file);
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_PROFILE_CREATOR_H_
