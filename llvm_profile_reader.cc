#include "llvm_profile_reader.h"

#include <memory>
#include <string>
#include <utility>

#include "symbol_map.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ProfileData/FunctionId.h"
#include "llvm/ProfileData/SampleProfReader.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "base/logging.h"

namespace devtools_crosstool_autofdo {

const char *LLVMProfileReader::GetName(const llvm::StringRef &N) {
  return names_.insert(N.str()).first->c_str();
}

#if LLVM_VERSION_MAJOR >= 12
bool LLVMProfileReader::ReadFromFile(
    const std::string &filename,
    llvm::sampleprof::FSDiscriminatorPass discriminator_pass) {
#else
bool LLVMProfileReader::ReadFromFile(const std::string &filename) {
#endif
  llvm::LLVMContext C;
#if LLVM_VERSION_MAJOR >= 12
  llvm::sampleprof::FunctionSamples::ProfileIsFS = false;
  auto reader_or_err = llvm::sampleprof::SampleProfileReader::create(
      filename, C,
#if LLVM_VERSION_MAJOR >= 17
      *llvm::vfs::getRealFileSystem(),
#endif
      discriminator_pass);
#else
  auto reader_or_err =
      llvm::sampleprof::SampleProfileReader::create(filename, C);
#endif
  if (!reader_or_err) {
    LOG(ERROR) << "Cannot create a SampleProfileReader: "
               << reader_or_err.getError().message();
    return false;
  }

  std::unique_ptr<llvm::sampleprof::SampleProfileReader> reader =
      std::move(reader_or_err.get());
  std::error_code read_error = reader->read();
  if (read_error != llvm::sampleprof_error::success) {
    LOG(ERROR) << "Cannot read profile: " << read_error.message();
    return false;
  }

  // LLVMProfileReader's profile symbol list will live longer than sample
  // profile reader, so need to use ProfileSymbolList::merge to copy the
  // underlying string data in reader Buffer into LLVMProfileReader's
  // profile symbol list.
  auto reader_sym_list = reader->getProfileSymbolList();
  if (reader_sym_list) {
    auto prof_sym_list =
        std::make_unique<llvm::sampleprof::ProfileSymbolList>();
    prof_sym_list->merge(*reader_sym_list);
    SetProfileSymbolList(std::move(prof_sym_list));
  }

  for (const auto &name_profile : reader->getProfiles()) {
    SourceStack stack;
    ReadFromFunctionSamples(stack, name_profile.second);
  }
#if LLVM_VERSION_MAJOR >= 12
  profile_is_fs_ = llvm::sampleprof::FunctionSamples::ProfileIsFS;
#endif
  return true;
}

void LLVMProfileReader::ReadFromFunctionSamples(
    const SourceStack &stack, const llvm::sampleprof::FunctionSamples &fs) {
  const char *func_name = GetName(fs.getFuncName());

  if (stack.empty() && !shouldMergeProfileForSym(func_name)) return;

  const char *top_func_name = nullptr;
  if (stack.empty()) {
    top_func_name = func_name;
    symbol_map_->AddSymbol(func_name);
    symbol_map_->AddSymbolEntryCount(func_name, fs.getHeadSamples());
  } else {
    top_func_name = stack.back().func_name;
  }
  for (const auto &loc_sample : fs.getBodySamples()) {
    SourceInfo info(func_name, "", "", 0, loc_sample.first.LineOffset,
                    loc_sample.first.Discriminator);
    SourceStack new_stack;
    new_stack.push_back(info);
    new_stack.insert(new_stack.end(), stack.begin(), stack.end());
    symbol_map_->AddSourceCount(top_func_name, new_stack,
                                loc_sample.second.getSamples(), 1);
    for (const auto &target_count : loc_sample.second.getCallTargets()) {
      symbol_map_->AddIndirectCallTarget(
          top_func_name, new_stack, GetName(target_count.first.stringRef()),
          target_count.second);
    }
  }
  for (const auto &loc_fsmap : fs.getCallsiteSamples()) {
    SourceInfo info(func_name, "", "", 0, loc_fsmap.first.LineOffset,
                    loc_fsmap.first.Discriminator);
    SourceStack new_stack;
    new_stack.push_back(info);
    new_stack.insert(new_stack.end(), stack.begin(), stack.end());
    for (const auto &name_fs : loc_fsmap.second)
      ReadFromFunctionSamples(new_stack, name_fs.second);
  }

  // The total count for a top-level function can be non-zero but the body
  // samples may be zero if there is no debug information for the function.
  // In this case, the total counts are populated but pos_counts is left empty.
  // Please see b/175733095 for more details.
  // NB: For inline instances, this can theoritically happen if lines without
  // debug information receive samples and lines with debug information don't.
  // It's not something we have seen in practice so it's not being implemented.
  if (stack.empty() &&
      symbol_map_->map().at(func_name)->total_count == 0) {
    symbol_map_->AddSymbolEntryCount(func_name, 0, fs.getTotalSamples());
  }
}

// Return whether to read the samples from current profile for the
// input symbol. Those symbols are having binary or file scope but
// with name conflict.
bool LLVMProfileReader::shouldMergeProfileForSym(const std::string name) {
  // If no special_syms_ specified, merge profile for every symbol.
  if (!special_syms_) return true;

  if (special_syms_->skip_set.find(name) != special_syms_->skip_set.end())
    return false;

  for (const std::string &sym_name : special_syms_->strip_all)
    if (name.compare(0, sym_name.length(), sym_name) == 0) return false;

  for (const std::string &sym_name : special_syms_->keep_sole) {
    // If the symbol doesn't exist in symbol_map_, return true so the
    // profile for the symbol will be added to symbol_map_. Next time
    // we see the same symbol in another profile, remove it from
    // symbol_map_ and add it to skip_set to skip the rest of the
    // profiles.
    if (name.compare(0, sym_name.length(), sym_name) == 0) {
      if (!symbol_map_->GetSymbolByName(name)) return true;
      symbol_map_->RemoveSymbol(name);
      special_syms_->skip_set.insert(name);
      return false;
    }
  }
  for (const std::string &sym_name : special_syms_->keep_cold) {
    if (name.compare(0, sym_name.length(), sym_name) == 0) {
      // Add a cold profile into symbol_map_ the first time we see
      // the symbol and add the symbol to skip_set to skip the rest
      // of the profiles.
      if (!symbol_map_->GetSymbolByName(name)) {
        symbol_map_->AddSymbol(name);
        symbol_map_->AddSymbolEntryCount(name, 0, kMinSamples + 1);
        special_syms_->skip_set.insert(name);
      }
      return false;
    }
  }
  return true;
}

}  // namespace devtools_crosstool_autofdo
