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

// Create AutoFDO profile.

#ifndef AUTOFDO_PROFILE_CREATOR_H_
#define AUTOFDO_PROFILE_CREATOR_H_

#include "addr2line.h"
#include "sample_reader.h"
#include "symbol_map.h"
#include "profile_writer.h"

namespace autofdo {

class ProfileCreator {
 public:
  explicit ProfileCreator(const string &binary)
      : sample_reader_(nullptr),
        binary_(binary),
        use_discriminator_encoding_(false) {}

  ~ProfileCreator() {
    delete sample_reader_;
  }

  void set_use_discriminator_encoding(bool use_discriminator_encoding) {
    use_discriminator_encoding_ = use_discriminator_encoding;
  }

  // Returns the total sample counts from a text profile.
  static uint64 GetTotalCountFromTextProfile(const string &input_profile_name);

  // Creates AutoFDO profile, returns true if success, false otherwise.
  bool CreateProfile(const string &input_profile_name, const string &profiler,
                     autofdo::ProfileWriter *writer,
                     const string &output_profile_name);

  // Reads samples from the input profile.
  bool ReadSample(const string &input_profile_name,
                  const string &profiler);

  // Creates output profile after reading from the input profile.
  bool CreateProfileFromSample(autofdo::ProfileWriter *writer,
                               const string &output_name);

  // Returns total number of samples collected.
  uint64 TotalSamples();

  // Returns the SampleReader pointer.
  const autofdo::SampleReader &sample_reader() {
    return *sample_reader_;
  }

  // Computes the profile and updates the given symbol map and addr2line
  // instance.
  bool ComputeProfile(autofdo::SymbolMap *symbol_map,
                      autofdo::Addr2line **addr2line);

 private:
  SampleReader *sample_reader_;
  string binary_;
  bool use_discriminator_encoding_;
};

bool MergeSample(const string &input_file, const string &input_profiler,
                 const string &binary, const string &output_file);
}  // namespace autofdo

#endif  // AUTOFDO_PROFILE_CREATOR_H_
