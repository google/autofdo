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
  static void Write(FILE *outf, const SymbolMap &symbol_map) {
    LLVMSourceProfileWriter writer(outf);
    writer.Start(symbol_map);
  }

 protected:
  void WriteSourceLocation(uint32 offset) {
    if (offset & 0xffff) {
      fprintf(outf_, "%u.%u: ", (offset >> 16), offset & 0xffff);
    } else {
      fprintf(outf_, "%u: ", (offset >> 16));
    }
  }

  void WriteIndentation() {
    for (int i = 0; i < level_; i++) {
      fprintf(outf_, " ");
    }
  }

  void VisitTopSymbol(const string &name, const Symbol *node) override {
    fprintf(outf_, "%s:%llu:%llu\n", name.c_str(), node->total_count,
            node->head_count);
  }

  void VisitCallsite(const Callsite &callsite) override {
    WriteIndentation();
    WriteSourceLocation(callsite.first);
    if (strlen(callsite.second) == 0)
      fprintf(outf_, "noname:");
    else
      fprintf(outf_, "%s:", callsite.second);
  }


  void Visit(const Symbol *node) override {
    if (level_ > 1) {
      fprintf(outf_, "%llu\n", node->total_count);
    }
    // Sort sample locations by line number.
    vector<uint32> positions;
    for (const auto &pos_count : node->pos_counts) {
      positions.push_back(pos_count.first);
    }

    // We have samples inside the function body. Write them out.
    std::sort(positions.begin(), positions.end());

    // Emit all the locations and their sample counts.
    for (const auto &pos : positions) {
      PositionCountMap::const_iterator ret = node->pos_counts.find(pos);
      DCHECK(ret != node->pos_counts.end());
      WriteIndentation();
      WriteSourceLocation(pos);
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
  explicit LLVMSourceProfileWriter(FILE *outf) : outf_(outf) {}

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
  LLVMSourceProfileWriter::Write(outf_, symbol_map_);
}

void LLVMProfileWriter::WriteFinish() {
  fclose(outf_);
}

bool LLVMProfileWriter::WriteToFile(const string &output_filename) {
  if (FLAGS_debug_dump) Dump();
  outf_ = WriteHeader(output_filename);
  if (!outf_) return false;
  WriteProfile();
  WriteFinish();
  return true;
}

}  // namespace autofdo
