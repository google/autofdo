// Copyright 2013 Google Inc. All Rights Reserved.
// Author: dnovillo@google.com (Diego Novillo)

// Convert a Perf profile to LLVM.

#include <cstdint>
#if defined(HAVE_LLVM)
#include <stdio.h>

#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "llvm_profile_writer.h"
#include "profile_writer.h"
#include "source_info.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/flags/declare.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/match.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ProfileData/FunctionId.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/ProfileData/SampleProfWriter.h"

ABSL_DECLARE_FLAG(bool, debug_dump);

namespace devtools_crosstool_autofdo {

bool LLVMProfileBuilder::Write(
    const std::string &output_filename,
    llvm::sampleprof::SampleProfileFormat format, const SymbolMap &symbol_map,
    const StringIndexMap &name_table,
    llvm::sampleprof::SampleProfileWriter *sample_profile_writer) {
  // Collect the profiles for every symbol in the name table.
  LLVMProfileBuilder builder(name_table);
  const auto &profiles = builder.ConvertProfiles(symbol_map);

#if LLVM_VERSION_MAJOR >= 12
  // Tell the profile writer if FS Discriminators are used.
  llvm::sampleprof::FunctionSamples::ProfileIsFS =
      SourceInfo::use_fs_discriminator;
#endif

  if (profiles.empty()) {
    LOG(WARNING) << "Got an empty profile map. The output file might still "
                    "be not empty (e.g., containing symbol list in binary "
                    "format) but might be not helpful as a profile";
  }
  // Write all the gathered profiles to the output file.
  if (std::error_code EC = sample_profile_writer->write(profiles)) {
    LOG(ERROR) << "Error writing profile output to '" << output_filename
               << "': " << EC.message();
    return false;
  }

  sample_profile_writer->getOutputStream().flush();
  return true;
}

// LLVM_BEFORE_SAMPLEFDO_SPLIT_CONTEXT is defined when llvm version is before
// https://reviews.llvm.org/rGb9db70369b7799887b817e13109801795e4d70fc
#ifndef LLVM_BEFORE_SAMPLEFDO_SPLIT_CONTEXT
const llvm::sampleprof::SampleProfileMap &LLVMProfileBuilder::ConvertProfiles(
    const SymbolMap &symbol_map) {
#else
const llvm::StringMap<llvm::sampleprof::FunctionSamples> &
LLVMProfileBuilder::ConvertProfiles(const SymbolMap &symbol_map) {
#endif
  Start(symbol_map);
  return GetProfiles();
}

void LLVMProfileBuilder::VisitTopSymbol(const std::string &name,
                                        const Symbol *node) {
  llvm::StringRef name_ref = GetNameRef(name);
  llvm::sampleprof::FunctionSamples &profile = profiles_[name_ref];
  if (std::error_code EC = llvm::mergeSampleProfErrors(
          result_, profile.addHeadSamples(node->head_count)))
    LOG(FATAL) << "Error updating head samples for '" << name
               << "': " << EC.message();

  if (std::error_code EC = llvm::mergeSampleProfErrors(
          result_, profile.addTotalSamples(node->total_count)))
    LOG(FATAL) << "Error updating total samples for '" << name
               << "': " << EC.message();

  profile.setFunction(llvm::sampleprof::FunctionId(name_ref));
  inline_stack_.clear();
  inline_stack_.push_back(&profile);
}

void LLVMProfileBuilder::VisitCallsite(const Callsite &callsite) {
  DCHECK_GE(inline_stack_.size(), 1);
  uint64_t offset = callsite.location;
  uint32_t line = SourceInfo::GetLineNumberFromOffset(offset);
  uint32_t discriminator = SourceInfo::GetDiscriminatorFromOffset(offset);
  while (inline_stack_.size() > level_) {
    inline_stack_.pop_back();
  }
  auto &caller_profile = *(inline_stack_.back());
  llvm::sampleprof::FunctionId Callee(
      GetNameRef(Symbol::Name(callsite.callee_name)));
  auto &callee_profile =
      caller_profile.functionSamplesAt(llvm::sampleprof::LineLocation(
          line, discriminator))[llvm::sampleprof::FunctionId(Callee)];
  callee_profile.setFunction(Callee);
  inline_stack_.push_back(&callee_profile);
}

void LLVMProfileBuilder::Visit(const Symbol *node) {
  DCHECK_GE(inline_stack_.size(), 1);
  auto &profile = *(inline_stack_.back());

  if (level_ > 1) {
    // If this is a nested inline call, update its total count.
    if (std::error_code EC = llvm::mergeSampleProfErrors(
            result_, profile.addTotalSamples(node->total_count)))
      LOG(FATAL) << "Error updating total samples for '" << node->info.func_name
                 << "': " << EC.message();
  }

  // Emit all the locations and their sample counts.
  for (const auto &pos_count : node->pos_counts) {
    uint64_t offset = pos_count.first;
    uint32_t line = SourceInfo::GetLineNumberFromOffset(offset);
    uint32_t discriminator = SourceInfo::GetDiscriminatorFromOffset(offset);
    const auto &num_samples = pos_count.second.count;
    if (std::error_code EC = llvm::mergeSampleProfErrors(
            result_, profile.addBodySamples(line, discriminator, num_samples)))
      LOG(FATAL) << "Error updating body samples for '" << node->info.func_name
                 << "': " << EC.message();

    // If there is a call at this location, emit the possible
    // targets. For direct calls, this will be the exact function
    // being invoked. For indirect calls, this will be a list of one
    // or more functions.
    const auto &target_map = pos_count.second.target_map;
    for (const auto &target_count : target_map) {
      if (std::error_code EC = llvm::mergeSampleProfErrors(
              result_,
              profile.addCalledTargetSamples(
                  line, discriminator,
                  llvm::sampleprof::FunctionId(llvm::StringRef(
                      target_count.first.data(), target_count.first.size())),
                  target_count.second)))
        LOG(FATAL) << "Error updating called target samples for '"
                   << node->info.func_name << "': " << EC.message();
    }
  }
}

llvm::StringRef LLVMProfileBuilder::GetNameRef(const std::string &str) {
  StringIndexMap::const_iterator ret =
      name_table_.find(Symbol::Name(str.c_str()));
  CHECK(ret != name_table_.end());
  // Suffixes should have been elided by SymbolMap::ElideSuffixesAndMerge()
  if (absl::StrContains(ret->first, ".llvm.")) {
    LOG(WARNING)
        << "Unexpected character '.' in function name: " << ret->first
        << ". Likely thin LTO .llvm.<hash> suffix has not been cleared.";
  }
  return llvm::StringRef(ret->first);
}

llvm::sampleprof::SampleProfileWriter *LLVMProfileWriter::CreateSampleWriter(
    const std::string &output_filename) {
  auto WriterOrErr = llvm::sampleprof::SampleProfileWriter::create(
      llvm::StringRef(output_filename), format_);
  if (std::error_code EC = WriterOrErr.getError()) {
    LOG(ERROR) << "Error creating profile output file '" << output_filename
               << "': " << EC.message();
    return nullptr;
  }
  sample_prof_writer_ = std::move(WriterOrErr.get());
  return sample_prof_writer_.get();
}

bool LLVMProfileWriter::WriteToFile(const std::string &output_filename) {
  if (absl::GetFlag(FLAGS_debug_dump)) Dump();

  // Populate the symbol table. This table contains all the symbols
  // for functions found in the binary.
  StringIndexMap name_table;
  FileIndexMap file_table;
  StringTableUpdater::Update(*symbol_map_, &name_table, &file_table);

  // If the underlying llvm profile writer has not been created yet,
  // create it here.
  if (!sample_prof_writer_) {
    if (!CreateSampleWriter(output_filename)) {
      return false;
    }
  }

  // Gather profiles for all the symbols.
  return LLVMProfileBuilder::Write(output_filename, format_, *symbol_map_,
                                   name_table, sample_prof_writer_.get());
}

}  // namespace devtools_crosstool_autofdo

#endif  // HAVE_LLVM
