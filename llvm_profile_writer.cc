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

// Convert a Perf profile to LLVM.

#include <stdio.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gflags/gflags.h"
#include "glog/logging.h"
#include "profile_writer.h"

DECLARE_bool(debug_dump);

namespace autofdo {

class LLVMSourceProfileWriter : public SymbolTraverser {
 public:
  static void Write(FILE *outf, const SymbolMap &symbol_map,
                    const StringIndexMap &map) {
    LLVMSourceProfileWriter writer(outf, map);
    writer.Start(symbol_map);
  }

 protected:
  void WriteSourceLocation(uint32 start_line, uint32 offset) {
    if (offset & 0xffff) {
      fprintf(outf_, "%u.%u: ", (offset >> 16) + start_line, offset & 0xffff);
    } else {
      fprintf(outf_, "%u: ", (offset >> 16) + start_line);
    }
  }

  virtual void Visit(const Symbol *node) {
    // Sort sample locations by line number.
    vector<uint32> positions;
    for (const auto &pos_count : node->pos_counts) {
      // Do not waste storage writing 0 counts.
      if (pos_count.second.count == 0) {
        continue;
      }
      positions.push_back(pos_count.first);
    }

    // Similarly, do not waste time emitting profiles for
    // functions that have no counts in them.
    if (positions.empty())
      return;

    // Clang does not generate a name for the implicit ctor of anonymous
    // structs, so there won't be a name to attach the samples to. If
    // the name of this function is empty, ignore it.
    if (strlen(node->info.func_name) == 0)
      return;

    // We have samples inside the function body. Write them out.
    sort(positions.begin(), positions.end());
    fprintf(outf_, "%s:%llu:%llu\n", node->info.func_name, node->total_count,
            node->head_count);

    // Emit all the locations and their sample counts.
    for (const auto &pos : positions) {
      PositionCountMap::const_iterator ret = node->pos_counts.find(pos);
      DCHECK(ret != node->pos_counts.end());
      WriteSourceLocation(node->info.start_line, pos);
      fprintf(outf_, "%llu", ret->second.count);

      // If there is a call at this location, emit the possible
      // targets. For direct calls, this will be the exact function
      // being invoked. For indirect calls, this will be a list of one
      // or more functions.
      TargetCountPairs target_count_pairs;
      GetSortedTargetCountPairs(ret->second.target_map, &target_count_pairs);
      for (const auto &target_count : target_count_pairs) {
        fprintf(outf_, "  %s:%llu", target_count.first.c_str(),
                target_count.second);
      }
      fprintf(outf_, "\n");
    }
  }

 private:
  explicit LLVMSourceProfileWriter(FILE *outf, const StringIndexMap &map)
      : map_(map), outf_(outf) {}

  int GetStringIndex(const string &str) {
    StringIndexMap::const_iterator ret = map_.find(str);
    CHECK(ret != map_.end());
    return ret->second;
  }

  const StringIndexMap &map_;
  FILE *outf_;
  DISALLOW_COPY_AND_ASSIGN(LLVMSourceProfileWriter);
};

FILE *LLVMProfileWriter::WriteHeader(const string &output_filename) {
  FILE *outf = fopen(output_filename.c_str(), "w");
  if (!outf) {
    LOG(ERROR) << "Could not open %s for writing.";
    return NULL;
  }
  return outf;
}

void LLVMProfileWriter::WriteProfile() {
  // Populate the symbol table. This table contains all the symbols
  // for functions found in the binary.
  StringIndexMap string_index_map;
  StringTableUpdater::Update(symbol_map_, &string_index_map);
  LLVMSourceProfileWriter::Write(outf_, symbol_map_, string_index_map);
}

void LLVMProfileWriter::WriteFinish() { fclose(outf_); }

bool LLVMProfileWriter::WriteToFile(const string &output_filename) {
  if (FLAGS_debug_dump) Dump();
  outf_ = WriteHeader(output_filename);
  if (!outf_) return false;
  WriteProfile();
  WriteFinish();
  return true;
}

}  // namespace autofdo
