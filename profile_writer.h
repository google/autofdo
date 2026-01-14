// Copyright 2012 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to build AutoFDO profile.

#ifndef AUTOFDO_PROFILE_WRITER_H_
#define AUTOFDO_PROFILE_WRITER_H_

#include <cstdint>
#include <map>
#include <string>

#include "symbol_map.h"

namespace devtools_crosstool_autofdo {

class SymbolMap;

class ProfileWriter {
 public:
  explicit ProfileWriter(const SymbolMap *symbol_map)
      : symbol_map_(symbol_map) {}
  explicit ProfileWriter() : symbol_map_(nullptr) {}
  virtual ~ProfileWriter() {}

  virtual bool WriteToFile(const std::string &output_file) = 0;
  void setSymbolMap(const SymbolMap *symbol_map) { symbol_map_ = symbol_map; }
  void Dump();

 protected:
  const SymbolMap *symbol_map_;
};

class AutoFDOProfileWriter : public ProfileWriter {
 public:
  explicit AutoFDOProfileWriter(const SymbolMap *symbol_map,
                                uint32_t gcov_version)
      : ProfileWriter(symbol_map), gcov_version_(gcov_version) {}
  explicit AutoFDOProfileWriter(uint32_t gcov_version)
      : gcov_version_(gcov_version) {}

  bool WriteToFile(const std::string &output_file) override;

 private:
  // Opens the output file, and writes the header.
  bool WriteHeader(const std::string &output_file);

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

  // Writes the module grouping info into the gcda file.
  // TODO(b/132437226): LIPO has been deprecated so no module grouping info
  // is needed in the gcda file. However, even if no LIPO is used, gcc used
  // by chromeOS kernel will still check the module grouping fields whenever
  // it reads a gcda file. To be compatible, we keep the minimum fields which
  // are necessary for gcc to be able to read a gcda file and remove the
  // rest of LIPO stuff.
  // We can remove the leftover if chromeOS kernel starts using llvm or can
  // change their gcc in sync with autofdo tool.
  //
  // The minimum fields to keep:
  // TAG
  // Length of the section (will always be 0)
  // Number of modules (will always be 0)
  void WriteModuleGroup();

  // Writes working set to gcov file.
  void WriteWorkingSet();

  uint32_t gcov_version_;
};

class SymbolTraverser {
 public:
  // This type is neither copyable nor movable.
  SymbolTraverser(const SymbolTraverser &) = delete;
  SymbolTraverser &operator=(const SymbolTraverser &) = delete;

  virtual ~SymbolTraverser() {}

 protected:
  SymbolTraverser() : level_(0) {}
  virtual void Start(const SymbolMap &symbol_map) {
    for (const auto &name_symbol : symbol_map.map()) {
      if (!symbol_map.ShouldEmit(name_symbol.second->total_count)) {
        continue;
      }
      VisitTopSymbol(name_symbol.first, name_symbol.second);
      Traverse(name_symbol.second);
    }
  }
  virtual void VisitTopSymbol(const std::string &name, const Symbol *node) {}
  virtual void Visit(const Symbol *node) = 0;
  virtual void VisitCallsite(const Callsite &offset) {}
  int level_;

 private:
  void Traverse(const Symbol *node) {
    level_++;
    Visit(node);
    for (const auto &callsite_symbol : node->callsites) {
      VisitCallsite(callsite_symbol.first);
      Traverse(callsite_symbol.second);
    }
    level_--;
  }
};

typedef std::map<std::string, int> StringIndexMap;

// Store a mapping from function names back to file names (with faster lookup
// using file_map_ -> file_names_). The ordering of file_names_ is not stable,
// it can vary across invocations depending on the order of symbol traversal.
class FileIndexMap {
  // The de-duplicated list of file names found in the program.
  std::vector<std::string> file_names_;
  // The map from function name string to an index in file_names_.
  std::unordered_map<std::string, uint32_t> name_map_;
  // The map from file name string to an index in file_names_.
  std::unordered_map<std::string, uint32_t> file_map_;

 public:
  int GetFileIndex(const std::string &symbol_name) {
    if (auto it = name_map_.find(symbol_name); it != name_map_.end())
      return it->second;
    else
      return -1;
  }

  std::string_view GetFileName(const std::string &symbol_name) {
    if (auto it = name_map_.find(symbol_name); it != name_map_.end())
      return file_names_[it->second];
    else
      return "";
  }

  // Add a new function name -> file name mapping. This de-duplicates the file
  // names by re-assigning the same index if the file name already exists.
  void AddFileName(const std::string &symbol_name,
                   const std::string &file_name) {
    if (auto it = file_map_.find(file_name); it != file_map_.end()) {
      name_map_[symbol_name] = it->second;
    } else {
      file_names_.emplace_back(file_name);
      file_map_[file_names_.back()] = file_names_.size() - 1;
      name_map_[symbol_name] = file_names_.size() - 1;
    }
  }

  size_t Size() const {
    return file_names_.size();
  }

  const std::vector<std::string> &GetFileNames() const {
    return file_names_;
  }
};

class StringTableUpdater: public SymbolTraverser {
 public:
  // This type is neither copyable nor movable.
  StringTableUpdater(const StringTableUpdater &) = delete;
  StringTableUpdater &operator=(const StringTableUpdater &) = delete;

  static void Update(const SymbolMap &symbol_map, StringIndexMap *map,
                     FileIndexMap *file_map) {
    StringTableUpdater updater(map, file_map);
    updater.Start(symbol_map);
  }

 protected:
  void Visit(const Symbol *node) override {
    if (node->info.func_name != nullptr) {
      if (node->info.dir_name != "") {
        file_map_->AddFileName(node->info.func_name,
                               std::filesystem::path(node->info.dir_name) /
                                   node->info.file_name);
      } else {
        file_map_->AddFileName(node->info.func_name, node->info.file_name);
      }
    }
    for (const auto &pos_count : node->pos_counts) {
      for (const auto &name_count : pos_count.second.target_map) {
        (*map_)[name_count.first] = 0;
      }
    }
  }

  void VisitCallsite(const Callsite &callsite) override {
    (*map_)[Symbol::Name(callsite.callee_name)] = 0;
  }

  void VisitTopSymbol(const std::string &name, const Symbol *node) override {
    (*map_)[Symbol::Name(name.c_str())] = 0;
  }

 private:
   explicit StringTableUpdater(StringIndexMap *map, FileIndexMap *file_map)
       : map_(map), file_map_(file_map) {}
   StringIndexMap *map_;
   FileIndexMap *file_map_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_PROFILE_WRITER_H_
