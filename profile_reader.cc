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

#include "base/common.h"
#include "addr2line.h"
#include "gcov.h"
#include "symbol_map.h"
#include "module_grouper.h"
#include "profile_reader.h"

namespace autofdo {

void AutoFDOProfileReader::ReadModuleGroup() {
  // Only update the module_map if its original size is 0.
  bool update_module_map = module_map_ ? module_map_->size() == 0 : false;
  CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_MODULE_GROUPING);
  gcov_read_unsigned();
  uint32 num_modules = gcov_read_unsigned();
  for (uint32 i = 0; i < num_modules; i++) {
    Module module;
    string name = gcov_read_string();
    module.is_exported = gcov_read_unsigned();
    module.lang = gcov_read_unsigned();
    module.ggc_memory_in_kb = gcov_read_unsigned();
    uint32 size = gcov_read_unsigned();
    module.num_quote_paths = gcov_read_unsigned();
    module.num_bracket_paths = gcov_read_unsigned();
    module.num_system_paths = gcov_read_unsigned();
    module.num_cpp_defines = gcov_read_unsigned();
    module.num_cpp_includes = gcov_read_unsigned();
    module.num_cl_args = gcov_read_unsigned();
    for (uint32 j = 0; j < size; j++) {
      module.aux_modules.insert(gcov_read_string());
    }
    for (int j = 0; j < module.num_quote_paths; j++) {
      module.options.push_back(Option(QUOTE_PATHS, gcov_read_string()));
    }
    for (int j = 0; j < module.num_bracket_paths; j++) {
      module.options.push_back(Option(BRACKET_PATHS, gcov_read_string()));
    }
    for (int j = 0; j < module.num_system_paths; j++) {
      module.options.push_back(Option(SYSTEM_PATHS, gcov_read_string()));
    }
    for (int j = 0; j < module.num_cpp_defines; j++) {
      module.options.push_back(Option(CPP_DEFINES, gcov_read_string()));
    }
    for (int j = 0; j < module.num_cpp_includes; j++) {
      module.options.push_back(Option(CPP_INCLUDES, gcov_read_string()));
    }
    for (int j = 0; j < module.num_cl_args; j++) {
      module.options.push_back(Option(CL_ARGS, gcov_read_string()));
    }
    if (update_module_map) {
      module_map_->insert(ModuleMap::value_type(name, module));
    }
  }
}

void AutoFDOProfileReader::ReadFunctionProfile() {
  CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_AFDO_FUNCTION);
  gcov_read_unsigned();
  uint32 num_functions = gcov_read_unsigned();
  SourceStack stack;
  for (uint32 i = 0; i < num_functions; i++) {
    ReadSymbolProfile(stack);
  }
}

void AutoFDOProfileReader::ReadSymbolProfile(const SourceStack &stack) {
  uint64 head_count;
  if (stack.size() == 0) {
    head_count = gcov_read_counter();
  } else {
    head_count = 0;
  }
  const char *name = names_.at(gcov_read_unsigned()).c_str();
  uint32 num_pos_counts = gcov_read_unsigned();
  uint32 num_callsites = gcov_read_unsigned();
  if (stack.size() == 0) {
    symbol_map_->AddSymbol(name);
    symbol_map_->AddSymbolEntryCount(name, head_count);
  }
  for (int i = 0; i < num_pos_counts; i++) {
    uint32 offset = gcov_read_unsigned();
    uint32 num_targets = gcov_read_unsigned();
    uint64 count = gcov_read_counter();
    SourceInfo info(name, NULL, NULL, 0, offset >> 16, offset & 0xffff);
    SourceStack new_stack;
    new_stack.push_back(info);
    new_stack.insert(new_stack.end(), stack.begin(), stack.end());
    symbol_map_->AddSourceCount(new_stack[new_stack.size() - 1].func_name,
                                new_stack, count, 1, SymbolMap::SUM);
    for (int j = 0; j < num_targets; j++) {
      // Only indirect call target histogram is supported now.
      CHECK_EQ(gcov_read_unsigned(), HIST_TYPE_INDIR_CALL_TOPN);
      const string &target_name = names_.at(gcov_read_counter());
      uint64 target_count = gcov_read_counter();
      symbol_map_->AddIndirectCallTarget(
          new_stack[new_stack.size() - 1].func_name,
          new_stack, target_name, target_count);
    }
  }
  for (int i = 0; i < num_callsites; i++) {
    // offset is encoded as:
    //   higher 16 bits: line offset to the start of the function.
    //   lower 16 bits: discriminator.
    uint32 offset = gcov_read_unsigned();
    SourceInfo info(name, NULL, NULL, 0, offset >> 16, offset & 0xffff);
    SourceStack new_stack;
    new_stack.push_back(info);
    new_stack.insert(new_stack.end(), stack.begin(), stack.end());
    ReadSymbolProfile(new_stack);
  }
}

void AutoFDOProfileReader::ReadNameTable() {
  CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_AFDO_FILE_NAMES);
  gcov_read_unsigned();
  uint32 name_vector_size = gcov_read_unsigned();
  for (uint32 i = 0; i < name_vector_size; i++) {
    names_.push_back(gcov_read_string());
  }
}

void AutoFDOProfileReader::ReadWorkingSet() {
  CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_AFDO_WORKING_SET);
  gcov_read_unsigned();
  for (uint32 i = 0; i < NUM_GCOV_WORKING_SETS; i++) {
    uint32 num_counters = gcov_read_unsigned();
    uint64 min_counter = gcov_read_counter();
    symbol_map_->UpdateWorkingSet(
        i, num_counters * WORKING_SET_INSN_PER_BB, min_counter);
  }
}

void AutoFDOProfileReader::ReadFromFile(const string &filename) {
  CHECK_NE(gcov_open(filename.c_str(), 1), -1);

  // Read tags
  CHECK_EQ(gcov_read_unsigned(), GCOV_DATA_MAGIC);
  CHECK_EQ(gcov_read_unsigned(), FLAGS_gcov_version);
  gcov_read_unsigned();

  ReadNameTable();
  ReadFunctionProfile();
  ReadModuleGroup();
  ReadWorkingSet();

  CHECK(!gcov_close());
}
}  // namespace autofdo
