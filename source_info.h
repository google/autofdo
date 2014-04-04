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

// Class to represent the source info.

#ifndef AUTOFDO_SOURCE_INFO_H_
#define AUTOFDO_SOURCE_INFO_H_

#include <vector>

namespace autofdo {

// Represents the source position.
struct SourceInfo {
  SourceInfo()
      : func_name(NULL), dir_name(NULL), file_name(NULL), start_line(0),
        line(0), discriminator(0) {}

  SourceInfo(const char *func_name, const char *dir_name, const char *file_name,
           uint32 start_line, uint32 line, uint32 discriminator)
      : func_name(func_name), dir_name(dir_name), file_name(file_name),
        start_line(start_line), line(line), discriminator(discriminator) {}

  bool operator<(const SourceInfo &p) const;

  string RelativePath() const {
    if (dir_name && *dir_name)
      return string(dir_name) + "/" + string(file_name);
    if (file_name)
      return string(file_name);
    return string();
  }

  uint32 Offset() const {
    return ((line - start_line) << 16) | discriminator;
  }

  bool Malformed() const {
    return line < start_line;
  }

  const char *func_name;
  const char *dir_name;
  const char *file_name;
  uint32 start_line;
  uint32 line;
  uint32 discriminator;
};

typedef vector<SourceInfo> SourceStack;
}  // namespace autofdo

#endif  // AUTOFDO_SOURCE_INFO_H_
