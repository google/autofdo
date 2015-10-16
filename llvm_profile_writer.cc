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

inline uint64_t SPMagic() {
  return uint64_t('S') << (64 - 8) | uint64_t('P') << (64 - 16) |
         uint64_t('R') << (64 - 24) | uint64_t('O') << (64 - 32) |
         uint64_t('F') << (64 - 40) | uint64_t('4') << (64 - 48) |
         uint64_t('2') << (64 - 56) | uint64_t(0xff);
}

inline uint64_t SPVersion() { return 102; }

inline void encodeULEB128(FILE *f, uint64 val) {
  do {
    uint8 byte = val & 0x7f;
    val >>= 7;
    if (val != 0)
      byte |= 0x80;  // Mark to indicate that more bytes will follow.
    fputc(char(byte), f);
  } while (val != 0);
}

class LLVMSourceProfileWriter : public SymbolTraverser {
 public:
  static void Write(FILE *outf, const SymbolMap &symbol_map,
                    const StringIndexMap &name_table) {
    LLVMSourceProfileWriter writer(outf, name_table);
    writer.Start(symbol_map);
  }

 protected:
  void WriteSourceLocation(uint32 offset) {
    encodeULEB128(outf_, offset >> 16);
    encodeULEB128(outf_, offset & 0xffff);
  }

  void VisitTopSymbol(const string &name, const Symbol *node) override {
    encodeULEB128(outf_, node->head_count);
    encodeULEB128(outf_, GetStringIndex(name));
    encodeULEB128(outf_, node->total_count);
  }

  void VisitCallsite(const Callsite &callsite) override {
    WriteSourceLocation(callsite.first);
    encodeULEB128(outf_, GetStringIndex(callsite.second));
  }


  void Visit(const Symbol *node) override {
    if (level_ > 1) {
      encodeULEB128(outf_, node->total_count);
    }

    // Sort sample locations by line number.
    vector<uint32> positions;
    for (const auto &pos_count : node->pos_counts) {
      positions.push_back(pos_count.first);
    }

    // We have samples inside the function body. Write them out.
    std::sort(positions.begin(), positions.end());

    // Emit the size of the location list.
    encodeULEB128(outf_, positions.size());

    // Emit all the locations and their sample counts.
    for (const auto &pos : positions) {
      PositionCountMap::const_iterator ret = node->pos_counts.find(pos);
      DCHECK(ret != node->pos_counts.end());

      // Write the source location and number of samples collected.
      WriteSourceLocation(pos);
      encodeULEB128(outf_, ret->second.count);

      // If there is a call at this location, emit the possible
      // targets. For direct calls, this will be the exact function
      // being invoked. For indirect calls, this will be a list of one
      // or more functions.
      TargetCountPairs target_count_pairs;
      GetSortedTargetCountPairs(ret->second.target_map, &target_count_pairs);
      encodeULEB128(outf_, target_count_pairs.size());
      for (const auto &target_count : target_count_pairs) {
        encodeULEB128(outf_, GetStringIndex(target_count.first));
        encodeULEB128(outf_, target_count.second);
      }
    }

    // Emit the number of callsites inline into this function. The samples for
    // each inlined callee will be written in the symbol map traveral calling
    // this visitor function.
    encodeULEB128(outf_, node->callsites.size());
  }

 private:
  explicit LLVMSourceProfileWriter(FILE *outf, const StringIndexMap &name_table)
      : outf_(outf), name_table_(name_table) {}

  int GetStringIndex(const string &str) {
    StringIndexMap::const_iterator ret = name_table_.find(str);
    CHECK(ret != name_table_.end());
    return ret->second;
  }

  FILE *outf_;
  const StringIndexMap &name_table_;
  DISALLOW_COPY_AND_ASSIGN(LLVMSourceProfileWriter);
};

bool LLVMProfileWriter::WriteHeader(const string &output_filename) {
  outf_ = fopen(output_filename.c_str(), "w");
  if (!outf_) {
    LOG(ERROR) << "Could not open " << output_filename << " for writing.";
    return false;
  }
  encodeULEB128(outf_, SPMagic());
  encodeULEB128(outf_, SPVersion());
  return true;
}

void LLVMProfileWriter::WriteProfile() {
  // Populate the symbol table. This table contains all the symbols
  // for functions found in the binary.
  StringIndexMap name_table;
  StringTableUpdater::Update(symbol_map_, &name_table);

  // Write the name table.
  encodeULEB128(outf_, name_table.size());
  unsigned last_name_index = 0;
  for (auto &name_index : name_table) {
    name_index.second = last_name_index++;
    fprintf(outf_, "%s", name_index.first.c_str());
    encodeULEB128(outf_, 0);
  }

  // Write the profiles.
  LLVMSourceProfileWriter::Write(outf_, symbol_map_, name_table);
}

void LLVMProfileWriter::WriteFinish() {
  fclose(outf_);
}

bool LLVMProfileWriter::WriteToFile(const string &output_filename) {
  if (FLAGS_debug_dump) Dump();
  if (!WriteHeader(output_filename)) return false;
  WriteProfile();
  WriteFinish();
  return true;
}

}  // namespace autofdo
