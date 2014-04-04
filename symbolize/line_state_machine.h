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

#ifndef AUTOFDO_SYMBOLIZE_LINE_STATE_MACHINE_H__
#define AUTOFDO_SYMBOLIZE_LINE_STATE_MACHINE_H__

namespace autofdo {

// This is the format of a DWARF2/3 line state machine that we process
// opcodes using.  There is no need for anything outside the lineinfo
// processor to know how this works.
struct LineStateMachine {
  void Reset(bool default_is_stmt) {
    file_num = 1;
    address = 0;
    line_num = 1;
    column_num = 0;
    discriminator = 0;
    is_stmt = default_is_stmt;
    basic_block = false;
    end_sequence = false;
  }

  uint32 file_num;
  uint64 address;
  uint64 line_num;
  uint32 column_num;
  uint32 discriminator;
  bool is_stmt;  // stmt means statement.
  bool basic_block;
  bool end_sequence;
};

}  // namespace autofdo


#endif  // AUTOFDO_SYMBOLIZE_LINE_STATE_MACHINE_H__
