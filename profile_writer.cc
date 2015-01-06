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

// Write profile to afdo file.

#include <stdio.h>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

#include "gflags/gflags.h"
#include "base/common.h"
#include "gcov.h"
#include "symbol_map.h"
#include "module_grouper.h"
#include "profile_writer.h"

// sizeof(gcov_unsigned_t)
#define SIZEOF_UNSIGNED 4

DEFINE_bool(debug_dump, false,
            "If set, emit additional debugging dumps to stderr.");

namespace autofdo {
// Opens the output file, and writes the header.
bool AutoFDOProfileWriter::WriteHeader(const string &output_filename,
                                       const string &imports_filename) {
  if (gcov_open(output_filename.c_str(), -1) == -1) {
    LOG(FATAL) << "Cannot open file " << output_filename;
    return false;
  }

  imports_file_ = fopen(imports_filename.c_str(), "w");
  if (imports_file_ == NULL) {
    LOG(FATAL) << "Cannot open file " << imports_filename;
    return false;
  }

  gcov_write_unsigned(GCOV_DATA_MAGIC);
  gcov_write_unsigned(gcov_version_);
  gcov_write_unsigned(0);
  return true;
}

// Finishes writing, closes the output file.
bool AutoFDOProfileWriter::WriteFinish() {
  if (gcov_close()) {
    LOG(ERROR) << "Cannot close the gcov file.";
    return false;
  }
  fclose(imports_file_);
  return true;
}

class SourceProfileLengther: public SymbolTraverser {
 public:
  explicit SourceProfileLengther(const SymbolMap &symbol_map)
      : length_(0), num_functions_(0) {
    Start(symbol_map);
  }

  int length() {return length_ + num_functions_ * 2;}
  int num_functions() {return num_functions_;}

 protected:
  virtual void VisitTopSymbol(const string &name, const Symbol *node) {
    num_functions_++;
  }

  virtual void Visit(const Symbol *node) {
    // func_name, num_pos_counts, num_callsites
    length_ += 3;
    // offset_discr, num_targets, count * 2
    length_ += node->pos_counts.size() * 4;
    // offset_discr
    length_ += node->callsites.size();
    for (const auto &pos_count : node->pos_counts) {
      // type, func_name * 2, count * 2
      length_ += pos_count.second.target_map.size() * 5;
    }
  }

 private:
  int length_;
  int num_functions_;
  DISALLOW_COPY_AND_ASSIGN(SourceProfileLengther);
};

class SourceProfileWriter: public SymbolTraverser {
 public:
  static void Write(const SymbolMap &symbol_map, const StringIndexMap &map) {
    SourceProfileWriter writer(map);
    writer.Start(symbol_map);
  }

 protected:
  virtual void Visit(const Symbol *node) {
    gcov_write_unsigned(node->pos_counts.size());
    gcov_write_unsigned(node->callsites.size());
    for (const auto &pos_count : node->pos_counts) {
      gcov_write_unsigned(pos_count.first);
      gcov_write_unsigned(pos_count.second.target_map.size());
      gcov_write_counter(pos_count.second.count);
      TargetCountPairs target_counts;
      GetSortedTargetCountPairs(pos_count.second.target_map, &target_counts);
      for (const auto &target_count : pos_count.second.target_map) {
        gcov_write_unsigned(HIST_TYPE_INDIR_CALL_TOPN);
        gcov_write_counter(GetStringIndex(target_count.first));
        gcov_write_counter(target_count.second);
      }
    }
  }

  virtual void VisitTopSymbol(const string &name, const Symbol *node) {
    gcov_write_counter(node->head_count);
    gcov_write_unsigned(GetStringIndex(name));
  }

  virtual void VisitCallsite(const Callsite &callsite) {
    gcov_write_unsigned(callsite.first);
    gcov_write_unsigned(GetStringIndex(
        callsite.second ? callsite.second : string()));
  }

 private:
  explicit SourceProfileWriter(const StringIndexMap &map) : map_(map) {}

  int GetStringIndex(const string &str) {
    StringIndexMap::const_iterator ret = map_.find(str);
    CHECK(ret != map_.end());
    return ret->second;
  }

  const StringIndexMap &map_;
  DISALLOW_COPY_AND_ASSIGN(SourceProfileWriter);
};

void AutoFDOProfileWriter::WriteFunctionProfile() {
  typedef map<string, int> StringIndexMap;
  // Map from a string to its index in this map. Providing a partial
  // ordering of all output strings.
  StringIndexMap string_index_map;
  int length_4bytes = 0, current_name_index = 0;
  string_index_map[string()] = 0;

  StringTableUpdater::Update(symbol_map_, &string_index_map);

  for (auto &name_index : string_index_map) {
    name_index.second = current_name_index++;
    length_4bytes += (name_index.first.size()
                      + SIZEOF_UNSIGNED) / SIZEOF_UNSIGNED;
    length_4bytes += 1;
  }
  length_4bytes += 1;

  // Writes the GCOV_TAG_AFDO_FILE_NAMES section.
  gcov_write_unsigned(GCOV_TAG_AFDO_FILE_NAMES);
  gcov_write_unsigned(length_4bytes);
  gcov_write_unsigned(string_index_map.size());
  for (const auto &name_index : string_index_map) {
    char *c = strdup(name_index.first.c_str());
    int len = strlen(c);
    // Workaround https://gcc.gnu.org/bugzilla/show_bug.cgi?id=64346
    // We should not have D4Ev in our profile because it does not exist
    // in symbol table and would lead to undefined symbols during linking.
    if (len > 5 &&
        (!strcmp(c + len - 4, "D4Ev") || !strcmp(c + len - 4, "C4Ev"))) {
      c[len - 3] = '2';
    }
    gcov_write_string(c);
    free(c);
  }

  // Compute the length of the GCOV_TAG_AFDO_FUNCTION section.
  SourceProfileLengther length(symbol_map_);
  gcov_write_unsigned(GCOV_TAG_AFDO_FUNCTION);
  gcov_write_unsigned(length.length() + 1);
  gcov_write_unsigned(length.num_functions());
  SourceProfileWriter::Write(symbol_map_, string_index_map);
}

void AutoFDOProfileWriter::WriteModuleGroup() {
  gcov_write_unsigned(GCOV_TAG_MODULE_GROUPING);

  // Calculates the length of the section.
  // Initial value: module count (an unsigned integer).
  int length_4bytes = 1;
  int num_modules = 0;
  // name_string_length, is_exported, has_sym, aux_module_size,
  // quote_paths_size, bracket_paths_size, system_paths_size,
  // cpp_defines_size, cpp_includes_size,
  // cl_args_size
  static const uint32 MODULE_AUX_DATA_SIZE_IN_4BYTES = 9;
  for (const auto &module_aux : module_map_) {
    if (!module_aux.second.is_exported
        && module_aux.second.aux_modules.size() == 0) {
      continue;
    }
    if (module_aux.second.is_fake) {
      continue;
    }
    num_modules++;
    length_4bytes += (module_aux.first.size() + SIZEOF_UNSIGNED)
                     / SIZEOF_UNSIGNED;
    length_4bytes += MODULE_AUX_DATA_SIZE_IN_4BYTES;
    length_4bytes += module_aux.second.aux_modules.size();
    for (const auto &aux : module_aux.second.aux_modules) {
      length_4bytes += (aux.size() + SIZEOF_UNSIGNED) / SIZEOF_UNSIGNED;
    }
    int num_string = module_aux.second.num_quote_paths
                   + module_aux.second.num_bracket_paths
                   + module_aux.second.num_system_paths
                   + module_aux.second.num_cpp_defines
                   + module_aux.second.num_cpp_includes
                   + module_aux.second.num_cl_args;
    length_4bytes += num_string;
    for (int i = 0; i < num_string; i++) {
      length_4bytes += (module_aux.second.options[i].second.size()
                        + SIZEOF_UNSIGNED) / SIZEOF_UNSIGNED;
    }
  }

  // Writes to .afdo file and .afdo.imports file.
  gcov_write_unsigned(length_4bytes);
  gcov_write_unsigned(num_modules);
  for (const auto &module_aux : module_map_) {
    if (!module_aux.second.is_exported
        && module_aux.second.aux_modules.size() == 0) {
      continue;
    }
    if (module_aux.second.is_fake) {
      continue;
    }

    // Writes the name of the module.
    gcov_write_string(module_aux.first.c_str());
    if (module_aux.second.aux_modules.size() > 0) {
      fprintf(imports_file_, "%s:", module_aux.first.c_str());
    }

    // Writes the "is_exported" flag.
    if (module_aux.second.is_exported) {
      gcov_write_unsigned(1);
    } else {
      gcov_write_unsigned(0);
    }

    gcov_write_unsigned(module_aux.second.lang);
    gcov_write_unsigned(module_aux.second.ggc_memory_in_kb);

    // Writes the aux module info.
    gcov_write_unsigned(module_aux.second.aux_modules.size());
    gcov_write_unsigned(module_aux.second.num_quote_paths);
    gcov_write_unsigned(module_aux.second.num_bracket_paths);
    if (module_aux.second.has_system_paths_field)
      gcov_write_unsigned(module_aux.second.num_system_paths);
    gcov_write_unsigned(module_aux.second.num_cpp_defines);
    gcov_write_unsigned(module_aux.second.num_cpp_includes);
    gcov_write_unsigned(module_aux.second.num_cl_args);

    for (const auto &aux : module_aux.second.aux_modules) {
      gcov_write_string(aux.c_str());
      fprintf(imports_file_, " %s", aux.c_str());
    }
    fprintf(imports_file_, "\n");

    // Writes the options recorded in the elf section of the original binary.
    int option_offset = 0;
    for (int i = 0; i < module_aux.second.num_quote_paths; i++) {
      gcov_write_string(
          module_aux.second.options[option_offset++].second.c_str());
    }
    for (int i = 0; i < module_aux.second.num_bracket_paths; i++) {
      gcov_write_string(
          module_aux.second.options[option_offset++].second.c_str());
    }
    for (int i = 0; i < module_aux.second.num_system_paths; i++) {
      gcov_write_string(
          module_aux.second.options[option_offset++].second.c_str());
    }
    for (int i = 0; i < module_aux.second.num_cpp_defines; i++) {
      gcov_write_string(
          module_aux.second.options[option_offset++].second.c_str());
    }
    for (int i = 0; i < module_aux.second.num_cpp_includes; i++) {
      gcov_write_string(
          module_aux.second.options[option_offset++].second.c_str());
    }
    for (int i = 0; i < module_aux.second.num_cl_args; i++) {
      gcov_write_string(
          module_aux.second.options[option_offset++].second.c_str());
    }
  }
}

void AutoFDOProfileWriter::WriteWorkingSet() {
  gcov_write_unsigned(GCOV_TAG_AFDO_WORKING_SET);
  gcov_write_unsigned(3 * NUM_GCOV_WORKING_SETS);
  const gcov_working_set_info *working_set = symbol_map_.GetWorkingSets();
  for (int i = 0; i < NUM_GCOV_WORKING_SETS; i++) {
    gcov_write_unsigned(working_set[i].num_counters / WORKING_SET_INSN_PER_BB);
    gcov_write_counter(working_set[i].min_counter);
  }
}

bool AutoFDOProfileWriter::WriteToFile(const string &output_filename) {
  if (!WriteHeader(output_filename, output_filename + ".imports")) {
    return false;
  }
  WriteFunctionProfile();
  WriteModuleGroup();
  WriteWorkingSet();
  if (!WriteFinish()) {
    return false;
  }
  return true;
}


// Debugging support.  ProfileDumper emits a detailed dump of the contents
// of the input profile.
class ProfileDumper : public SymbolTraverser {
 public:
  static void Write(const SymbolMap &symbol_map, const StringIndexMap &map) {
    ProfileDumper writer(map);
    writer.Start(symbol_map);
  }

 protected:
  void DumpSourceInfo(SourceInfo info, int indent) {
    printf("%*sDirectory name: %s\n", indent, " ", info.dir_name);
    printf("%*sFile name:      %s\n", indent, " ", info.file_name);
    printf("%*sFunction name:  %s\n", indent, " ", info.func_name);
    printf("%*sStart line:     %u\n", indent, " ", info.start_line);
    printf("%*sLine:           %u\n", indent, " ", info.line);
    printf("%*sDiscriminator:  %u\n", indent, " ", info.discriminator);
  }

  void PrintSourceLocation(uint32 start_line, uint32 offset) {
    if (offset & 0xffff) {
      printf("%u.%u: ", (offset >> 16) + start_line, offset & 0xffff);
    } else {
      printf("%u: ", (offset >> 16) + start_line);
    }
  }

  virtual void Visit(const Symbol *node) {
    printf("Writing symbol: ");
    node->Dump(4);
    printf("\n");
    printf("Source information:\n");
    DumpSourceInfo(node->info, 0);
    printf("\n");
    printf("Total sampled count:            %llu\n",
           static_cast<uint64>(node->total_count));
    printf("Total sampled count in head bb: %llu\n",
           static_cast<uint64>(node->head_count));
    printf("\n");
    printf("Call sites:\n");
    int i = 0;
    for (const auto &callsite_symbol : node->callsites) {
      Callsite site = callsite_symbol.first;
      Symbol *symbol = callsite_symbol.second;
      printf("  #%d: site\n", i);
      printf("    uint32: %u\n", site.first);
      printf("    const char *: %s\n", site.second);
      printf("  #%d: symbol: ", i);
      symbol->Dump(0);
      printf("\n");
      i++;
    }

    printf("node->pos_counts.size() = %llu\n",
           static_cast<uint64>(node->pos_counts.size()));
    printf("node->callsites.size() = %llu\n",
           static_cast<uint64>(node->callsites.size()));
    vector<uint32> positions;
    for (const auto &pos_count : node->pos_counts)
      positions.push_back(pos_count.first);
    std::sort(positions.begin(), positions.end());
    i = 0;
    for (const auto &pos : positions) {
      PositionCountMap::const_iterator pos_count = node->pos_counts.find(pos);
      DCHECK(pos_count != node->pos_counts.end());
      uint32 location = pos_count->first;
      ProfileInfo info = pos_count->second;

      printf("#%d: location (line[.discriminator]) = ", i);
      PrintSourceLocation(node->info.start_line, location);
      printf("\n");
      printf("#%d: profile info execution count = %llu\n", i, info.count);
      printf("#%d: profile info number of instructions = %llu\n", i,
             info.num_inst);
      TargetCountPairs target_counts;
      GetSortedTargetCountPairs(info.target_map, &target_counts);
      printf("#%d: profile info target map size = %llu\n", i,
             static_cast<uint64>(info.target_map.size()));
      printf("#%d: info.target_map:\n", i);
      for (const auto &target_count : info.target_map) {
        printf("\tGetStringIndex(target_count.first): %d\n",
               GetStringIndex(target_count.first));
        printf("\ttarget_count.second: %llu\n", target_count.second);
      }
      printf("\n");
      i++;
    }
  }

  virtual void VisitTopSymbol(const string &name, const Symbol *node) {
    printf("VisitTopSymbol: %s\n", name.c_str());
    node->Dump(0);
    printf("node->head_count: %llu\n", node->head_count);
    printf("GetStringIndex(%s): %u\n", name.c_str(), GetStringIndex(name));
    printf("\n");
  }

  virtual void VisitCallsite(const Callsite &callsite) {
    printf("VisitCallSite: %s\n", callsite.second);
    printf("callsite.first: %u\n", callsite.first);
    printf("GetStringIndex(callsite.second): %u\n",
           GetStringIndex(callsite.second ? callsite.second : string()));
  }

 private:
  explicit ProfileDumper(const StringIndexMap &map) : map_(map) {}

  int GetStringIndex(const string &str) {
    StringIndexMap::const_iterator ret = map_.find(str);
    CHECK(ret != map_.end());
    return ret->second;
  }

  const StringIndexMap &map_;
  DISALLOW_COPY_AND_ASSIGN(ProfileDumper);
};

// Emit a dump of the input profile on stdout.
void ProfileWriter::Dump() {
  StringIndexMap string_index_map;
  StringTableUpdater::Update(symbol_map_, &string_index_map);
  SourceProfileLengther length(symbol_map_);
  printf("Length of symbol map: %d\n", length.length() + 1);
  printf("Number of functions:  %d\n", length.num_functions());
  ProfileDumper::Write(symbol_map_, string_index_map);
}

}  // namespace autofdo
