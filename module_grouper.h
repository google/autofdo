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

// These classes defined here are used to group modules. For each primary
// module, we will find its frequent callees, and group the callee module
// to the primary module.

#ifndef AUTOFDO_MODULE_GROUPER_H_
#define AUTOFDO_MODULE_GROUPER_H_

#include <string>
#include <map>
#include <set>
#include <vector>

#include "base/common.h"

namespace autofdo {

// This enum stores different types of module option information originally
// designed in LIPO.
enum OptionType {
  QUOTE_PATHS   = 0,
  BRACKET_PATHS = 1,
  CPP_DEFINES   = 2,
  CPP_INCLUDES  = 3,
  CL_ARGS       = 4,
  LIPO_INFO     = 5,
  SYSTEM_PATHS  = 6
};

class Function;
class SymbolMap;
class Symbol;
typedef pair<OptionType, string> Option;
typedef map<Function *, int64> EdgeCount;

// The structure to store the auxilary information for each module.
class Module {
 public:
  explicit Module() :
      num_quote_paths(0), num_bracket_paths(0), num_system_paths(0),
      num_cpp_defines(0), num_cpp_includes(0), num_cl_args(0),
      has_system_paths_field(false),
      is_exported(false), is_fake(false),
      is_valid(true), lang(0), ggc_memory_in_kb(0) {}
  explicit Module(bool is_fake) :
      num_quote_paths(0), num_bracket_paths(0), num_system_paths(0),
      num_cpp_defines(0), num_cpp_includes(0), num_cl_args(0),
      has_system_paths_field(false),
      is_exported(false), is_fake(is_fake),
      is_valid(true), lang(0), ggc_memory_in_kb(0) {}

  int num_quote_paths;
  int num_bracket_paths;
  int num_system_paths;
  int num_cpp_defines;
  int num_cpp_includes;
  int num_cl_args;
  // Binary compatibility flag -- crosstool v17 introduces
  // a new field in GCDA file to record system include paths.
  bool has_system_paths_field;
  // If the module is the auxilary module of other modules.
  bool is_exported;
  // If the module is a fake module.
  // Fake module is for representing header files.
  bool is_fake;
  // If the module has inconsistent compilation info.
  bool is_valid;
  // The lower 16 bits represent the programming language.
  // The higher 16 bits represent whether there is assembly in the module.
  // This is combined into a uint32 to conform with GCC representation.
  uint32 lang;
  // Total GC memory consumed by compiler in KiB.
  uint32 ggc_memory_in_kb;
  // The module option information originally designed in LIPO.
  vector<Option> options;
  // Paths of the auxilary modules.
  set<string> aux_modules;
  // Map from compiler commandline flag to value;
  map<string, bool> flag_values;
};

class Function {
 public:
  explicit Function(const string &name, const string &module) :
      name(name), module(module), total_in_count(0), total_out_count(0) {}

  void AddInEdgeCount(int64 count, Function *in_func);
  void AddOutEdgeCount(int64 count, Function *out_func);

  // Function name
  string name;
  // Module name
  string module;
  // Sum of count of all incoming edges.
  int64 total_in_count;
  // Sum of count of all outgoing edges.
  int64 total_out_count;
  // Map from all incoming edges to counts.
  EdgeCount in_edge_count;
  // Map from all outgoing edges to counts.
  EdgeCount out_edge_count;
};

class CallEdge {
 public:
  explicit CallEdge(Function *from, Function *to) : from(from), to(to) {}

  explicit CallEdge() : from(NULL), to(NULL) {}

  bool operator==(const CallEdge &edge) const {
    return from == edge.from && to == edge.to;
  }

  bool operator<(const CallEdge &edge) const {
    return from->name < edge.from->name ||
           (from->name == edge.from->name && to->name < edge.to->name);
  }

  Function *from;
  Function *to;
};

// Map from module name to Module
typedef map<string, Module> ModuleMap;

class ModuleGrouper {
  friend class ModuleGrouperTest;
 public:
  static ModuleGrouper *GroupModule(
      const string &binary,
      const string &section_prefix,
      const SymbolMap *symbol_map);
  ~ModuleGrouper() { }

  const ModuleMap &module_map() {
    return module_map_;
  }

 private:
  explicit ModuleGrouper(const SymbolMap *symbol_map)
      : total_count_(0), symbol_map_(symbol_map) {}

  // Adds auxilary modules for each primary module.
  void Group();

  // Reads from binary file to get the command line options.
  void ReadModuleOptions(const string &binary,
                         const string &section_prefix);

  // Reads from binary file to get the command line options
  // for a specific option type.
  void ReadOptionsByType(const string &binary,
                         const string &section_prefix,
                         OptionType type);
  // DFS the symbol tree to traverse all call edges in symbol.
  void RecursiveBuildGraph(const string &caller_name, const Symbol *symbol);
  // Builds the module map and computes the initial edge weights.
  void BuildGraph();
  // Adds count for a give edge.
  void AddEdgeCount(const CallEdge &edge, int64 count);
  // Integrates the edge as the auxilary module.
  void IntegrateEdge(const CallEdge &edge);
  // Returns the total memory used in the module and its aux modules.
  uint32 GetTotalMemory(const set<string> &modules);
  // Returns true if TO_MODULE should be integrated as aux module of
  // FROM_MODULE. Note: ShouldIntegrate(a, b) != ShouldIntegrate(b, a)
  bool ShouldIntegrate(const string &from_module, const string &to_module);
  // Finds the edge with the largest count. Returns the count.
  int64 GetMaxEdge(CallEdge *edge);
  // Returns the .cc file name of a given header file name.
  string ReplaceHeaderToCC(string header_name) const;
  // Returns the module name for a give file name, and updates the module_map.
  string UpdateModuleMap(string name);

  // Map from function name to Function
  typedef map<string, Function> FunctionMap;
  // Map from edge to count
  typedef map<CallEdge, int64> EdgeMap;

  int64 total_count_;
  ModuleMap module_map_;
  FunctionMap function_map_;
  EdgeMap edge_map_;
  const SymbolMap *symbol_map_;
  DISALLOW_COPY_AND_ASSIGN(ModuleGrouper);
};
}  // namespace autofdo

#endif  // AUTOFDO_MODULE_GROUPER_H_
