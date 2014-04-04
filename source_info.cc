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

#include "base/common.h"
#include "source_info.h"

namespace {
int StrcmpMaybeNull(const char *a, const char *b) {
  if (a == nullptr) {
    a = "";
  }
  if (b == nullptr) {
    b = "";
  }
  return strcmp(a, b);
}
}  // namespace

namespace autofdo {
bool SourceInfo::operator<(const SourceInfo &p) const {
  if (line != p.line) {
    return line < p.line;
  }
  if (start_line != p.start_line) {
    return start_line < p.start_line;
  }
  if (discriminator != p.discriminator) {
    return discriminator < p.discriminator;
  }
  int ret = StrcmpMaybeNull(func_name, p.func_name);
  if (ret != 0) {
    return ret < 0;
  }
  ret = StrcmpMaybeNull(file_name, p.file_name);
  if (ret != 0) {
    return ret < 0;
  }
  return StrcmpMaybeNull(dir_name, p.dir_name) < 0;
}
}  // namespace autofdo
