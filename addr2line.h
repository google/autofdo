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

// Class to derive inline stack.

#ifndef AUTOFDO_ADDR2LINE_H_
#define AUTOFDO_ADDR2LINE_H_

#include <string>
#include <map>

#include "base/common.h"
#include "source_info.h"

namespace autofdo {
class Addr2line {
 public:
  explicit Addr2line(const string &binary_name) : binary_name_(binary_name) {}

  virtual ~Addr2line() {}

  static Addr2line *Create(const string &binary_name);

  static Addr2line *CreateWithSampledFunctions(
      const string &binary_name, const map<uint64, uint64> *sampled_functions);

  // Reads the binary to prepare necessary binary in data.
  // Returns True on success.
  virtual bool Prepare() = 0;

  // Stores the inline stack of ADDR in STACK.
  virtual void GetInlineStack(uint64 addr, SourceStack *stack) const = 0;

 protected:
  string binary_name_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Addr2line);
};

class AddressQuery;
class InlineStackHandler;
class ElfReader;
class LineIdentifier;
typedef map<uint64, LineIdentifier> AddressToLineMap;

class Google3Addr2line : public Addr2line {
 public:
  explicit Google3Addr2line(const string &binary_name,
                            const map<uint64, uint64> *sampled_functions);
  virtual ~Google3Addr2line();
  virtual bool Prepare();
  virtual void GetInlineStack(uint64 address, SourceStack *stack) const;

 private:
  AddressToLineMap *line_map_;
  InlineStackHandler *inline_stack_handler_;
  ElfReader *elf_;
  const map<uint64, uint64> *sampled_functions_;
  DISALLOW_COPY_AND_ASSIGN(Google3Addr2line);
};
}  // namespace autofdo

#endif  // AUTOFDO_ADDR2LINE_H_
