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

// A simple example of how to use the DWARF2/3 reader to
// extract function and line information from the debug info.
// You very much do not want to just slurp up everything like this
// does.  You more likely want to only process things you are
// interested in.
// It is sane to build functioninfo for the entire program at once in
// most cases.
// It is usually insane to build a line map that consists of 32 bytes
// per line in the original program :)

#include "symbolize/functioninfo.h"

#include <map>
#include <vector>

#include "base/common.h"
#include "symbolize/dwarf2enums.h"

namespace autofdo {

CULineInfoHandler::CULineInfoHandler(FileVector* files,
                                     DirectoryVector* dirs,
                                     AddressToLineMap* linemap)
    : linemap_(linemap), files_(files), dirs_(dirs),
      sampled_functions_(NULL) {
  Init();
}

CULineInfoHandler::CULineInfoHandler(
    FileVector* files,
    DirectoryVector* dirs,
    AddressToLineMap* linemap,
    const map<uint64, uint64> *sampled_functions)
    : linemap_(linemap), files_(files), dirs_(dirs),
      sampled_functions_(sampled_functions) {
  Init();
}
void CULineInfoHandler::Init() {
  // The dirs and files are 1 indexed, so just make sure we put
  // nothing in the 0 vector.
  CHECK_EQ(dirs_->size(), 0);
  CHECK_EQ(files_->size(), 0);
  dirs_->push_back("");
  files_->push_back(make_pair(0, ""));
}

bool CULineInfoHandler::ShouldAddAddress(uint64 address) const {
  // Looks for the first entry above the given address, then decrement the
  // iterator, then check that it's within the range [start, start + len).
  if (sampled_functions_ == NULL) {
    return true;
  }
  map<uint64, uint64>::const_iterator iter = sampled_functions_->upper_bound(
      address);
  if (iter == sampled_functions_->begin()) {
    return false;
  }
  --iter;
  return address < iter->first + iter->second;
}

void CULineInfoHandler::DefineDir(const char *name, uint32 dir_num) {
  // These should never come out of order, actually
  CHECK_EQ(dir_num, dirs_->size());
  dirs_->push_back(name);
}

void CULineInfoHandler::DefineFile(const char *name,
                                   int32 file_num, uint32 dir_num,
                                   uint64 mod_time, uint64 length) {
  // These should never come out of order, actually.
  CHECK_GE(dir_num, 0);
  CHECK_LT(dir_num, dirs_->size());
  if (file_num == files_->size() || file_num == -1) {
    files_->push_back(make_pair(dir_num, name));
  } else {
    LOG(INFO) << "error in DefineFile";
  }
}

void CULineInfoHandler::AddLine(uint64 address, uint32 file_num,
                                uint32 line_num, uint32 column_num,
                                uint32 discriminator) {
  if (!ShouldAddAddress(address)) {
    return;
  }
  if (file_num < files_->size()) {
    const pair<int, const char *>& file = (*files_)[file_num];
    if (file.first < dirs_->size()) {
      DirectoryFilePair file_and_dir = make_pair((*dirs_)[file.first],
                                                 file.second);
      LineIdentifier line_id(file_and_dir, line_num, discriminator);
      (*linemap_)[address] = line_id;
    } else {
      LOG(INFO) << "error in AddLine (bad dir_num " << file.first << ")";
    }
  } else {
    LOG(INFO) << "error in AddLine (bad file_num " << file_num << ")";
  }
}

string CULineInfoHandler::MergedFilename(const pair<const char *,
                                         const char *>& filename) {
  string dir = filename.first;
  if (dir.empty())
    return filename.second;
  else
    return dir + "/" + filename.second;
}

bool CUFunctionInfoHandler::StartCompilationUnit(uint64 offset,
                                                 uint8 address_size,
                                                 uint8 offset_size,
                                                 uint64 cu_length,
                                                 uint8 dwarf_version) {
  return true;
}


// For function info, we only care about subprograms and inlined
// subroutines. For line info, the DW_AT_stmt_list lives in the
// compile unit tag.

bool CUFunctionInfoHandler::StartDIE(uint64 offset, enum DwarfTag tag,
                                     const AttributeList& attrs) {
  switch (tag) {
    case DW_TAG_subprogram:
    case DW_TAG_inlined_subroutine: {
      current_function_info_ = new FunctionInfo;
      current_function_info_->lowpc = current_function_info_->highpc = 0;
      current_function_info_->name = "";
      current_function_info_->line = 0;
      current_function_info_->file = make_pair("", "");
      offset_to_funcinfo_->insert(make_pair(offset, current_function_info_));
      FALLTHROUGH_INTENDED;
    }
    case DW_TAG_compile_unit:
      return true;
    default:
      return false;
  }
  return false;
}

// Only care about the name attribute for functions

void CUFunctionInfoHandler::ProcessAttributeString(uint64 offset,
                                                   enum DwarfAttribute attr,
                                                   enum DwarfForm form,
                                                   const char *data) {
  if (attr == DW_AT_name && current_function_info_)
    current_function_info_->name = data;
}

void CUFunctionInfoHandler::ProcessAttributeUnsigned(uint64 offset,
                                                     enum DwarfAttribute attr,
                                                     enum DwarfForm form,
                                                     uint64 data) {
  if (attr == DW_AT_stmt_list) {
    SectionMap::const_iterator iter = sections_.find(".debug_line");
    CHECK(iter != sections_.end());

    LineInfo lireader(iter->second.first + data, iter->second.second  - data,
                      reader_, linehandler_);
    lireader.Start();
  } else if (current_function_info_) {
    switch (attr) {
      case DW_AT_low_pc:
        current_function_info_->lowpc = data;
        break;
      case DW_AT_high_pc:
        current_function_info_->highpc = data;
        break;
      case DW_AT_decl_line:
        current_function_info_->line = data;
        break;
      case DW_AT_decl_file:
        if (data < files_->size()) {
          const FileVector::value_type& file = (*files_)[data];
          CHECK_LT(file.first, dirs_->size());
          const char *dir = (*dirs_)[file.first];
          current_function_info_->file = make_pair(dir, file.second);
        } else {
          LOG(INFO) << "unexpected file_num " << data;
        }
        break;
      default:
        break;
    }
  }
}

void CUFunctionInfoHandler::EndDIE(uint64 offset) {
  if (current_function_info_ && current_function_info_->lowpc)
    address_to_funcinfo_->insert(make_pair(current_function_info_->lowpc,
                                           current_function_info_));
}

}  // namespace autofdo
