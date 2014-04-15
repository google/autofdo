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

// Class to build AutoFDO profile.

#ifndef AUTOFDO_PROFILE_WRITER_H_
#define AUTOFDO_PROFILE_WRITER_H_

#include "module_grouper.h"
#include "symbol_map.h"

namespace autofdo {

class SymbolMap;

class ProfileWriter {
 public:
  explicit ProfileWriter(const SymbolMap &symbol_map,
                         const ModuleMap &module_map)
      : symbol_map_(symbol_map), module_map_(module_map) {}
  virtual ~ProfileWriter() {}

  virtual bool WriteToFile(const string &output_file) = 0;
  void Dump();

 protected:
  const SymbolMap &symbol_map_;
  const ModuleMap &module_map_;
};

class AutoFDOProfileWriter : public ProfileWriter {
 public:
  explicit AutoFDOProfileWriter(const SymbolMap &symbol_map,
                                const ModuleMap &module_map,
                                uint32 gcov_version)
      : ProfileWriter(symbol_map, module_map),
        gcov_version_(gcov_version) {}

  virtual bool WriteToFile(const string &output_file);

 private:
  // Opens the output file, and writes the header.
  bool WriteHeader(const string &output_file, const string &imports_file);

  // Finishes writing, closes the output file.
  bool WriteFinish();

  // Writes the function profile to the gcda file. The profile has two parts:
  // GCOV_TAG_AFDO_FILE_NAMES:
  //   String table that stores all the file names.
  // GCOV_TAG_AFDO_FUNCTION:
  //   Function table that stores all the function info:
  //   TAG
  //   Length of the section
  //   Number of functions
  //     Function_1: function name
  //     Function_1: file name
  //      ...
  //     Function_n: ...
  //
  // The new function profile format:
  // GCOV_TAG_AFDO_FILE_NAMES:
  //   String table that stores all the file names.
  // GCOV_TAG_AFDO_FUNCTION:
  //   Function table that stores all the function info:
  //   TAG
  //   Length of the section
  //   Number of functions
  //     symbol profile 1
  //     symbol profile 2
  //      ...
  //     symbol profile num_functions
  //
  // Symbol profile is an iterative structure:
  //
  // symbol profile: func_name, file_name, start_line,
  //                 num_pos_counts, num_callsites
  //   offset_1: num_targets, count
  //     target_1: count
  //   offset_2: num_targets, count
  //    ...
  //   offset_num_pos_counts:
  //   callsite_offset_1: symbol profile
  //   callsite_offset_2: symbol profile
  //    ...
  //   callsite_offset_num_callsites: symbol profile
  void WriteFunctionProfile();

  // Writes the module grouping info into the gcda file. This is stored
  // under the section tagged GCOV_TAG_MODULE_GROUPING:
  // TAG
  // Length of the section
  // Number of modules
  //  Module_1: name
  //  Module_1: is_exported
  //  Module_1: lang
  //  Module_1: ggc_memory_in_kb
  //  Module_1: number of aux-modules
  //  Module_1: number of quote paths
  //  Module_1: number of bracket_paths
  //  Module_1: number of system_paths [new in crosstool-17]
  //  Module_1: number of cpp_defines
  //  Module_1: number of cpp_includes
  //  Module_1: number of cl_args
  //    string_1
  //     ...
  //    string_n
  //   ...
  //  Module_n: ...
  void WriteModuleGroup();

  // Writes working set to gcov file.
  void WriteWorkingSet();

  FILE *imports_file_;
  uint32 gcov_version_;
};

class SymbolTraverser {
 public:
  virtual ~SymbolTraverser() {}

 protected:
  SymbolTraverser() {}
  virtual void Start(const SymbolMap &symbol_map) {
    for (const auto &name_symbol : symbol_map.map()) {
      if (name_symbol.second->total_count == 0) {
        continue;
      }
      VisitTopSymbol(name_symbol.first, name_symbol.second);
      Traverse(name_symbol.second);
    }
  }
  virtual void VisitTopSymbol(const string &name, const Symbol *node) {}
  virtual void Visit(const Symbol *node) = 0;
  virtual void VisitCallsite(const Callsite &offset) {}

 private:
  void Traverse(const Symbol *node) {
    Visit(node);
    for (const auto &callsite_symbol : node->callsites) {
      VisitCallsite(callsite_symbol.first);
      Traverse(callsite_symbol.second);
    }
  }
  DISALLOW_COPY_AND_ASSIGN(SymbolTraverser);
};

typedef map<string, int> StringIndexMap;

class StringTableUpdater: public SymbolTraverser {
 public:
  static void Update(const SymbolMap &symbol_map, StringIndexMap *map) {
    StringTableUpdater updater(map);
    updater.Start(symbol_map);
  }

 protected:
  virtual void Visit(const Symbol *node) {
    (*map_)[node->info.func_name ? node->info.func_name : string()] = 0;
    for (const auto &pos_count : node->pos_counts) {
      for (const auto &name_count : pos_count.second.target_map) {
        (*map_)[name_count.first] = 0;
      }
    }
  }

  virtual void VisitTopSymbol(const string &name, const Symbol *node) {
    (*map_)[name] = 0;
  }

 private:
  explicit StringTableUpdater(StringIndexMap *map) : map_(map) {}
  StringIndexMap *map_;
  DISALLOW_COPY_AND_ASSIGN(StringTableUpdater);
};

// Writer class for LLVM profiles. This writes a text file with a
// simple frequency-based profile.
class LLVMProfileWriter : public ProfileWriter {
 public:
  explicit LLVMProfileWriter(const SymbolMap &symbol_map,
                             const ModuleMap &module_map)
      : ProfileWriter(symbol_map, module_map) {}

  virtual bool WriteToFile(const string &output_filename);

 private:
  // Open the output file and write its header.
  FILE *WriteHeader(const string &output_filename);

  // Write the body of the profile in text format.
  //
  //    function1:total_samples:total_head_samples
  //    offset1[.discriminator]: number_of_samples [fn1:num fn2:num ... ]
  //    offset2[.discriminator]: number_of_samples [fn3:num fn4:num ... ]
  //    ...
  //    offsetN[.discriminator]: number_of_samples [fn5:num fn6:num ... ]
  //
  // Function names must be mangled in order for the profile loader to
  // match them in the current translation unit. The two numbers in the
  // function header specify how many total samples were accumulated in
  // the function (first number), and the total number of samples accumulated
  // at the prologue of the function (second number). This head sample
  // count provides an indicator of how frequent is the function invoked.
  //
  // Each sampled line may contain several items. Some are optional
  // (marked below):
  //
  // a- Source line offset. This number represents the line number
  //    in the function where the sample was collected. The line number
  //    is always relative to the line where symbol of the function
  //    is defined. So, if the function has its header at line 280,
  //    the offset 13 is at line 293 in the file.
  //
  // b- [OPTIONAL] Discriminator. This is used if the sampled program
  //    was compiled with DWARF discriminator support
  //    (http://wiki.dwarfstd.org/index.php?title=Path_Discriminators)
  //
  // c- Number of samples. This is the number of samples collected by
  //    the profiler at this source location.
  //
  // d- [OPTIONAL] Potential call targets and samples. If present, this
  //    line contains a call instruction. This models both direct and
  //    indirect calls. Each called target is listed together with the
  //    number of samples. For example,
  //
  //    130: 7  foo:3  bar:2  baz:7
  //
  //    The above means that at relative line offset 130 there is a
  //    call instruction that calls one of foo(), bar() and baz(). With
  //    baz() being the relatively more frequent call target.
  void WriteProfile();

  // Close the profile file and flush out any trailing data.
  void WriteFinish();

  FILE *outf_;

  DISALLOW_COPY_AND_ASSIGN(LLVMProfileWriter);
};
}  // namespace autofdo

#endif  // AUTOFDO_PROFILE_WRITER_H_
