#include "profile_reader.h"

#include <cstdint>
#include <string>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "addr2line.h"
#include "gcov.h"
#include "profile_writer.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/flags/flag.h"

namespace devtools_crosstool_autofdo {
AutoFDOProfileReader::~AutoFDOProfileReader() {
  if (summary_info_) {
    delete summary_info_;
  }
}

void AutoFDOProfileReader::ReadModuleGroup() {
  CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_MODULE_GROUPING);
  // Length of the section. Always 0.
  gcov_read_unsigned();
  // Number of modules. Always 0.
  gcov_read_unsigned();
}

void AutoFDOProfileReader::ReadFunctionProfile() {
  CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_AFDO_FUNCTION);
  gcov_read_unsigned();
  uint32_t num_functions = gcov_read_unsigned();
  SourceStack stack;
  for (uint32_t i = 0; i < num_functions; i++) {
    ReadSymbolProfile(stack, true);
  }
}

void AutoFDOProfileReader::ReadSymbolProfile(const SourceStack &stack,
                                             bool update) {
  uint64_t head_count;
  if (stack.size() == 0) {
    head_count = gcov_read_counter();
  } else {
    head_count = 0;
  }
  uint32_t name_index = gcov_read_unsigned();
  const char *name = names_.at(name_index).first.c_str();
  uint32_t file_index = names_.at(name_index).second;
  const std::string &file_name =
      file_index < file_names_.size() ? file_names_.at(file_index) : "";
  uint32_t num_pos_counts = gcov_read_unsigned();
  uint32_t num_callsites = gcov_read_unsigned();
  if (stack.size() == 0) {
    symbol_map_->AddSymbol(name);
    const_cast<Symbol *>(symbol_map_->GetSymbolByName(name))->info.file_name =
        file_name;
    if (!force_update_ && symbol_map_->GetSymbolByName(name)->total_count > 0) {
      update = false;
    }
    if (force_update_ || update) {
      symbol_map_->AddSymbolEntryCount(name, head_count);
    }
  }
  for (int i = 0; i < num_pos_counts; i++) {
    uint32_t offset = gcov_read_unsigned();
    uint32_t num_targets = gcov_read_unsigned();
    uint64_t count = gcov_read_counter();
    SourceInfo info(name, "", "", 0, offset >> 16, offset & 0xffff);
    info.file_name = file_name;
    SourceStack new_stack;
    new_stack.push_back(info);
    new_stack.insert(new_stack.end(), stack.begin(), stack.end());
    if (force_update_ || update) {
      symbol_map_->AddSourceCount(new_stack[new_stack.size() - 1].func_name,
                                  new_stack, count, 1);
    }
    for (int j = 0; j < num_targets; j++) {
      // Only indirect call target histogram is supported now.
      CHECK_EQ(gcov_read_unsigned(), HIST_TYPE_INDIR_CALL_TOPN);
      const std::string &target_name = names_.at(gcov_read_counter()).first;
      uint64_t target_count = gcov_read_counter();
      if (force_update_ || update) {
        symbol_map_->AddIndirectCallTarget(
            new_stack[new_stack.size() - 1].func_name,
            new_stack, target_name, target_count);
      }
    }
  }
  for (int i = 0; i < num_callsites; i++) {
    // offset is encoded as:
    //   higher 16 bits: line offset to the start of the function.
    //   lower 16 bits: discriminator.
    uint32_t offset = gcov_read_unsigned();
    SourceInfo info(name, "", "", 0, offset >> 16, offset & 0xffff);
    info.file_name = file_name;
    SourceStack new_stack;
    new_stack.push_back(info);
    new_stack.insert(new_stack.end(), stack.begin(), stack.end());
    ReadSymbolProfile(new_stack, update);
  }
}

void AutoFDOProfileReader::ReadSummary() {
  if (absl::GetFlag(FLAGS_gcov_version) >= 3) {
    ProfileSummaryInformation info;
    CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_AFDO_SUMMARY);
    info.total_count_ = gcov_read_counter(); // Total count
    info.max_count_ = gcov_read_counter(); // Max count
    info.max_function_count_ = gcov_read_counter(); // Max function count
    info.num_counts_ = gcov_read_counter(); // Num counts
    info.num_functions_ = gcov_read_counter(); // Num functions
    unsigned num = gcov_read_counter();
    info.detailed_summaries_.resize(num);
    for (unsigned i = 0; i < num; i++) {
      info.detailed_summaries_[i].cutoff_ = gcov_read_unsigned(); // Cutoff
      info.detailed_summaries_[i].min_count_ = gcov_read_counter();  // Min count
      info.detailed_summaries_[i].num_counts_ = gcov_read_counter();  // Num counts >= min count
    }
    summary_info_ = new ProfileSummaryInformation(info);
  }
}

void AutoFDOProfileReader::ReadNameTable() {
  CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_AFDO_FILE_NAMES);
  gcov_read_unsigned();
  if (absl::GetFlag(FLAGS_gcov_version) >= 3) {
    uint32_t file_name_vector_size = gcov_read_unsigned();
    for (uint32_t i = 0; i < file_name_vector_size; i++) {
      file_names_.push_back(gcov_read_string());
    }
  }
  uint32_t name_vector_size = gcov_read_unsigned();
  for (uint32_t i = 0; i < name_vector_size; i++) {
    const char *name = gcov_read_string();
    uint32_t file_index =
        absl::GetFlag(FLAGS_gcov_version) >= 3 ? gcov_read_unsigned() : -1;
    names_.emplace_back(name, file_index);
  }
}

void AutoFDOProfileReader::ReadWorkingSet() {
  CHECK_EQ(gcov_read_unsigned(), GCOV_TAG_AFDO_WORKING_SET);
  gcov_read_unsigned();
  for (uint32_t i = 0; i < NUM_GCOV_WORKING_SETS; i++) {
    uint32_t num_counters = gcov_read_unsigned();
    uint64_t min_counter = gcov_read_counter();
    symbol_map_->UpdateWorkingSet(
        i, num_counters * WORKING_SET_INSN_PER_BB, min_counter);
  }
}

bool AutoFDOProfileReader::ReadFromFile(const std::string &output_file) {
  CHECK_NE(gcov_open(output_file.c_str(), 1), -1);

  // Read tags
  CHECK_EQ(gcov_read_unsigned(), GCOV_DATA_MAGIC) << output_file;
  absl::SetFlag(&FLAGS_gcov_version, gcov_read_unsigned());
  gcov_read_unsigned();

  ReadSummary();
  ReadNameTable();
  ReadFunctionProfile();
  ReadModuleGroup();
  ReadWorkingSet();

  CHECK(!gcov_close());

  return true;
}

ProfileSummaryInformation *AutoFDOProfileReader::GetSummaryInformation() const {
  return summary_info_;
}

}  // namespace devtools_crosstool_autofdo
