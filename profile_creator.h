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

#include "sample_reader.h"

namespace autofdo {

class ProfileCreator {
 public:
  explicit ProfileCreator(const string &binary) : sample_reader_(NULL),
                                                  binary_(binary) {}

  ~ProfileCreator() {
    delete sample_reader_;
  }

  // Returns the total sample counts from a text profile.
  static uint64 GetTotalCountFromTextProfile(const string &input_profile_name);

  // Creates AutoFDO profile, returns true if success, false otherwise.
  bool CreateProfile(const string &input_profile_name,
                     const string &profiler,
                     const string &output_profile_name,
                     const string &output_format);

  // Reads samples from the input profile.
  bool ReadSample(const string &input_profile_name,
                  const string &profiler);

  // Creates output profile after reading from the input profile.
  bool CreateProfileFromSample(const string &output_profile_name,
                               const string &output_format);

  // Returns total number of samples collected.
  uint64 TotalSamples();

  // Returns the SampleReader pointer.
  const autofdo::SampleReader &sample_reader() {
    return *sample_reader_;
  }

 private:
  SampleReader *sample_reader_;
  string binary_;
};

bool MergeSample(const string &input_file, const string &input_profiler,
                 const string &binary, const string &output_file);
}  // namespace autofdo

#endif  // AUTOFDO_PROFILE_CREATOR_H_
