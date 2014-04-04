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

#include "module_grouper.h"

#include "gflags/gflags.h"
#include "base/common.h"
#include "symbolize/bytereader-inl.h"
#include "symbolize/elf_reader.h"
#include "symbol_map.h"

DEFINE_int32(cutoff_percent, 85,
             "The module group will stop when total number of integrated call "
             "edge counts sums to more than this percentage of total call edge "
             "count");
DEFINE_int32(max_ggc_memory, 3 << 20,
             "The maximum ggc memory (in KB) of each module. This indicates "
             "how much memory GCC can be used to build each module. The "
             "default value is 3GB because hip-forge cluster enforce memory "
             "consumption to be less than 3GB");

namespace autofdo {
// in_func is not a const pointer, but it's not modified in the function.
void Function::AddInEdgeCount(int64 count, Function *in_func) {
  pair<EdgeCount::iterator, bool> ret = in_edge_count.insert(
      EdgeCount::value_type(in_func, 0));
  ret.first->second += count;
  total_in_count += count;
}

// out_func is not a const pointer, but it's not modified in the function.
void Function::AddOutEdgeCount(int64 count, Function *out_func) {
  pair<EdgeCount::iterator, bool> ret = out_edge_count.insert(
      EdgeCount::value_type(out_func, 0));
  ret.first->second += count;
  total_out_count += count;
}

ModuleGrouper *ModuleGrouper::GroupModule(
      const string &binary,
      const string &section_prefix,
      const SymbolMap *symbol_map) {
  ModuleGrouper *grouper = new ModuleGrouper(symbol_map);
  grouper->ReadModuleOptions(binary, section_prefix);
  if (grouper->module_map().size() == 0) {
    LOG(WARNING) << "Cannot read compilation info from binary. "
                 << "Please use -frecord-compilation-info-in-elf when "
                 << "building the binary";
  } else {
    grouper->Group();
  }
  return grouper;
}

void ModuleGrouper::Group() {
  BuildGraph();
  CallEdge max_edge;
  int64 target_accumulate_count = total_count_ * FLAGS_cutoff_percent / 100;
  map<string, set<string> > legacy_group;

  // Reads the legacy group from inline stacks (symbol_map.map())
  for (const auto &name_symbol : symbol_map_->map()) {
    const Symbol *symbol = name_symbol.second;
    if (symbol->IsFromHeader()) {
      continue;
    }
    const string base_module_name = symbol->ModuleName();
    vector<const Symbol *> queue;
    queue.push_back(symbol);
    while (!queue.empty()) {
      const Symbol *s = queue.back();
      queue.pop_back();
      for (const auto &pos_symbol : s->callsites) {
        queue.push_back(pos_symbol.second);
      }
      if (s->IsFromHeader() || s->ModuleName() == base_module_name ||
          s->total_count == 0) {
        continue;
      }
      legacy_group[base_module_name].insert(s->ModuleName());
    }
  }

  // Updates the legacy group to the new module group
  for (const auto &name_modules : legacy_group) {
    if (module_map_.find(name_modules.first) == module_map_.end()) {
      LOG(ERROR) << "Primary module " << name_modules.first
                 << " is not found in the profile binary";
      continue;
    }
    for (const auto &name : name_modules.second) {
      if (module_map_.find(name) == module_map_.end()) {
        LOG(ERROR) << "Module " << name.c_str()
                   << " is not found in the profile binary";
        continue;
      }
      module_map_[name].is_exported = true;
    }
    module_map_[name_modules.first].aux_modules.insert(
        name_modules.second.begin(), name_modules.second.end());
  }

  for (int64 accumulate_count = GetMaxEdge(&max_edge);
       accumulate_count < target_accumulate_count;
       accumulate_count += GetMaxEdge(&max_edge)) {
    if (edge_map_[max_edge] == 0) {
      break;
    }
    // Check if module should be integrated as aux-module.
    if (ShouldIntegrate(max_edge.from->module, max_edge.to->module)) {
      IntegrateEdge(max_edge);
    } else {
      // The edge has been processed and edge count should been decreased so
      // that it will not be processed any more.
      AddEdgeCount(max_edge, edge_map_[max_edge] * -1);
    }
  }
}

string ModuleGrouper::ReplaceHeaderToCC(string header_name) const {
  // Header file name is something like: ./mustang/hit-iterator.h
  // We change it to: mustang/hit-iterator.cc
  if (header_name.size() < 5 || header_name.substr(0, 2) != "./"
      || header_name.substr(header_name.size() - 2) != ".h") {
    return header_name;
  }
  return header_name.substr(2, header_name.size() - 3) + "cc";
}

string ModuleGrouper::UpdateModuleMap(string name) {
  if (module_map_.find(name) == module_map_.end()) {
    string new_name = ReplaceHeaderToCC(name);
    if (module_map_.find(new_name) != module_map_.end()) {
        return new_name;
    } else {
      module_map_.insert(ModuleMap::value_type(name, Module(true)));
    }
  }
  return name;
}

void ModuleGrouper::RecursiveBuildGraph(const string &caller_name,
                                        const Symbol *symbol) {
  const Symbol *caller = symbol_map_->GetSymbolByName(caller_name);
  CHECK(caller != NULL);
  for (const auto &pos_count : symbol->pos_counts) {
    for (const auto &target_count : pos_count.second.target_map) {
      const string &callee_name = target_count.first;
      const Symbol *callee = symbol_map_->GetSymbolByName(callee_name);
      if (callee == NULL || caller == callee) {
        continue;
      }
      total_count_ += target_count.second;
      string caller_module_name = UpdateModuleMap(caller->ModuleName());
      string callee_module_name = UpdateModuleMap(callee->ModuleName());
      pair<FunctionMap::iterator, bool> caller_ret =
          function_map_.insert(FunctionMap::value_type(
          caller_name, Function(caller_name, caller_module_name)));
      pair<FunctionMap::iterator, bool> callee_ret =
          function_map_.insert(FunctionMap::value_type(
          callee_name, Function(callee_name, callee_module_name)));
      AddEdgeCount(
          CallEdge(&(caller_ret.first->second), &(callee_ret.first->second)),
          target_count.second);
    }
  }
  for (const auto &callsite_symbol : symbol->callsites) {
    // For the inlined functions, the call will be attributed to the outermost
    // symbol, i.e. the symbol that is emitted in the profiled binary.
    RecursiveBuildGraph(caller_name, callsite_symbol.second);
  }
}

void ModuleGrouper::BuildGraph() {
  for (const auto &name_symbol : symbol_map_->map()) {
    RecursiveBuildGraph(name_symbol.first, name_symbol.second);
  }
}

void ModuleGrouper::AddEdgeCount(const CallEdge &edge, int64 count) {
  edge.from->AddOutEdgeCount(count, edge.to);
  edge.to->AddInEdgeCount(count, edge.from);
  pair<EdgeMap::iterator, bool> ret = edge_map_.insert(
      EdgeMap::value_type(edge, 0));
  ret.first->second += count;
}

// Integrates edge.to into edge.from.
// The integration has two steps:
// 1. Integrates the node (function) in the call graph.
//   Clones the the callees of edge.to to edge.from (with scaled count),
//   and decreases the counts in for edge.to's callee accordingly.
// 2. Integrates the module.
//   2.1 Adds edge.to.module.aux_modules to edge.from.module
//   2.2 Sets the is_exported field of edge.to.module
void ModuleGrouper::IntegrateEdge(const CallEdge &edge) {
  int64 count = edge_map_[edge];
  int64 total = edge.to->total_in_count;

  AddEdgeCount(edge, count * -1);
  for (auto &callee_count : edge.to->out_edge_count) {
    int64 scaled_count = callee_count.second * count / total;
    if (scaled_count > 0) {
      // Only add the scaled_count for non-recursive functions.
      if (callee_count.first != edge.from) {
        AddEdgeCount(CallEdge(edge.from, callee_count.first),
                     scaled_count);
      }
      // Decrease the scaled_count from the original callee.
      AddEdgeCount(CallEdge(edge.to, callee_count.first), scaled_count * -1);
    }
  }
  // Add the callee's module as the caller's module's aux-module.
  ModuleMap::iterator from_module_iter = module_map_.find(edge.from->module);
  ModuleMap::iterator to_module_iter = module_map_.find(edge.to->module);
  if (from_module_iter->first != to_module_iter->first) {
    if (!to_module_iter->second.is_fake) {
      from_module_iter->second.aux_modules.insert(to_module_iter->first);
    }
    from_module_iter->second.aux_modules.insert(
        to_module_iter->second.aux_modules.begin(),
        to_module_iter->second.aux_modules.end());
    to_module_iter->second.is_exported = true;
  }
}

uint32 ModuleGrouper::GetTotalMemory(const set<string> &modules) {
  uint32 ret = 0;
  for (const auto &module : modules) {
    ret += module_map_[module].ggc_memory_in_kb;
  }
  return ret;
}

// Data structure and content copied from gcc.
struct opt_desc {
  const char *opt_str;
  const char *opt_neg_str;
  bool default_val;
};

static struct opt_desc force_match_opts[] = {
  { "-fexceptions", "-fno-exceptions", true },
  { "-fsized-delete", "-fno-sized-delete", false },
  { "-frtti", "-fno-rtti", true },
  { "-fstrict-aliasing", "-fno-strict-aliasing", true },
  { "-fsigned-char", "-funsigned-char", true},
};

static bool HasIncompatibleArg(const Module &m1, const Module &m2) {
  for (int i = 0; i < sizeof(force_match_opts)/sizeof(opt_desc); i++) {
    if (m1.flag_values.find(force_match_opts[i].opt_str)->second !=
        m2.flag_values.find(force_match_opts[i].opt_str)->second) {
      return false;
    }
  }
  return true;
}

bool ModuleGrouper::ShouldIntegrate(const string &from_module,
                                    const string &to_module) {
  if (!module_map_[from_module].is_valid
      || !module_map_[to_module].is_valid) {
    return false;
  }
  if (from_module == to_module) {
    return true;
  }
  // Never integrate tcmalloc as auxilary module.
  if (to_module == "tcmalloc/tcmalloc_or_debug.cc"
      || to_module == "tcmalloc/tcmalloc.cc") {
    return false;
  }
  // We preprocess faked module first because it does not have lang field and
  // flag_values fields.
  if (!module_map_[to_module].is_fake && !module_map_[from_module].is_fake) {
    if ((module_map_[from_module].lang & 0xffff) !=
        (module_map_[to_module].lang & 0xffff)) {
      return false;
    }
    if (!HasIncompatibleArg(module_map_[from_module], module_map_[to_module])) {
      return false;
    }
  }
  set<string> modules;
  modules.insert(from_module);
  modules.insert(to_module);
  modules.insert(module_map_[from_module].aux_modules.begin(),
                 module_map_[from_module].aux_modules.end());
  modules.insert(module_map_[to_module].aux_modules.begin(),
                 module_map_[to_module].aux_modules.end());
  return GetTotalMemory(modules) < FLAGS_max_ggc_memory;
}

int64 ModuleGrouper::GetMaxEdge(CallEdge *edge) {
  // TODO(dehao): use more effective approach to search.
  int64 max_count = 0;
  for (const auto &edge_count : edge_map_) {
    if (edge_count.second > max_count ||
        (edge_count.second == max_count && edge_count.first < *edge)) {
      max_count = edge_count.second;
      *edge = edge_count.first;
    }
  }
  return max_count;
}

// Function that read in the options from the elf section.
void ModuleGrouper::ReadModuleOptions(const string &binary,
                                      const string &section_prefix) {
  ReadOptionsByType(binary, section_prefix, QUOTE_PATHS);
  ReadOptionsByType(binary, section_prefix, BRACKET_PATHS);
  ReadOptionsByType(binary, section_prefix, SYSTEM_PATHS);
  ReadOptionsByType(binary, section_prefix, CPP_DEFINES);
  ReadOptionsByType(binary, section_prefix, CPP_INCLUDES);
  ReadOptionsByType(binary, section_prefix, CL_ARGS);
  ReadOptionsByType(binary, section_prefix, LIPO_INFO);
}

// Reads in command-line options from the elf section by a specific type.
void ModuleGrouper::ReadOptionsByType(const string &binary,
                                      const string &section_prefix,
                                      OptionType type) {
  // Reads from the elf .note sections to get the option info
  // stored by the compiler.
  ElfReader elf(binary);
  const char *sect_data = NULL;
  size_t section_size;
  switch (type) {
  case QUOTE_PATHS:
    sect_data = elf.GetSectionByName(
        section_prefix + ".quote_paths", &section_size);
    break;
  case BRACKET_PATHS:
    sect_data = elf.GetSectionByName(
        section_prefix + ".bracket_paths", &section_size);
    break;
  case SYSTEM_PATHS:
    sect_data = elf.GetSectionByName(
        section_prefix + ".system_paths", &section_size);
    break;
  case CPP_DEFINES:
    sect_data = elf.GetSectionByName(
        section_prefix + ".cpp_defines", &section_size);
    break;
  case CPP_INCLUDES:
    sect_data = elf.GetSectionByName(
        section_prefix + ".cpp_includes", &section_size);
    break;
  case CL_ARGS:
    sect_data = elf.GetSectionByName(
        section_prefix + ".cl_args", &section_size);
    break;
  case LIPO_INFO:
    sect_data = elf.GetSectionByName(
        section_prefix + ".lipo_info", &section_size);
    break;
  }

  if (!sect_data || section_size == 0)
    return;

  // Iterates all sections to read in compilation info stored by compiler.
  for (const char *curr = sect_data; curr < sect_data + section_size;) {
    const char *module_name = curr;
    const char *num_ptr = curr + strlen(module_name) + 1;
    ByteReader reader(ENDIANNESS_LITTLE);
    size_t len;
    uint64 option_num =
        (type == LIPO_INFO) ? 0 : reader.ReadUnsignedLEB128(num_ptr, &len);
    std::pair<ModuleMap::iterator, bool> status = module_map_.insert(
        ModuleMap::value_type(module_name, Module()));
    Module *module = &status.first->second;
    bool is_duplicated = false;
    switch (type) {
    case QUOTE_PATHS:
      if (module->num_quote_paths > 0) {
        is_duplicated = true;
      } else {
        module->num_quote_paths = option_num;
      }
      break;
    case SYSTEM_PATHS:
      module->has_system_paths_field = true;
      if (!sect_data || section_size == 0)
        return;

      if (module->num_system_paths > 0) {
        is_duplicated = true;
      } else {
        module->num_system_paths = option_num;
      }
      break;
    case BRACKET_PATHS:
      if (module->num_bracket_paths > 0) {
        is_duplicated = true;
      } else {
        module->num_bracket_paths = option_num;
      }
      break;
    case CPP_DEFINES:
      if (module->num_cpp_defines > 0) {
        is_duplicated = true;
      } else {
        module->num_cpp_defines = option_num;
      }
      break;
    case CPP_INCLUDES:
      if (module->num_cpp_includes > 0) {
        is_duplicated = true;
      } else {
        module->num_cpp_includes = option_num;
      }
      break;
    case CL_ARGS:
      if (module->num_cl_args > 0) {
        is_duplicated = true;
      } else {
        module->num_cl_args = option_num;
      }
      break;
    case LIPO_INFO:
      size_t len1, len2;
      module->lang = reader.ReadUnsignedLEB128(num_ptr, &len1);
      module->ggc_memory_in_kb = reader.ReadUnsignedLEB128(num_ptr + len1,
                                                           &len2);
      len = len1 + len2;
      break;
    }

    for (int j = 0; j < sizeof(force_match_opts) / sizeof(opt_desc); j++) {
      module->flag_values[force_match_opts[j].opt_str] =
          force_match_opts[j].default_val;
    }

    curr = curr + strlen(curr) + 1 + len;
    for (int j = 0; j < option_num; j++) {
      if (!is_duplicated) {
        module->options.push_back(Option(type, curr));
        if (type == CL_ARGS) {
          for (int k = 0; k < sizeof(force_match_opts) / sizeof(opt_desc);
               k++) {
            // Checks if flag matches with the force_match_opts, if none is
            // matching, this flag does not need to be checked.
            if (!strcmp(force_match_opts[k].opt_str, curr)) {
              module->flag_values[force_match_opts[k].opt_str] = true;
            } else if (!strcmp(force_match_opts[k].opt_neg_str, curr)) {
              module->flag_values[force_match_opts[k].opt_str] = false;
            }
          }
        }
      } else if (module->options[module->options.size() - option_num + j].second
                 != curr) {
        module->is_valid = false;
        LOG(ERROR) << "Duplicated module entry for " << module_name;
        break;
      }
      curr += strlen(curr) + 1;
    }
  }
}
}  // namespace autofdo
