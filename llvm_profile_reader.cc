#include "llvm_profile_reader.h"

#include "symbol_map.h"
#include "llvm/ProfileData/SampleProfReader.h"

namespace devtools_crosstool_autofdo {

const char *LLVMProfileReader::GetName(const llvm::StringRef &N) {
  return names_.insert(N.str()).first->c_str();
}

bool LLVMProfileReader::ReadFromFile(const std::string &filename) {
  llvm::LLVMContext C;
  auto reader_or_err =
      llvm::sampleprof::SampleProfileReader::create(filename, C);
  if (reader_or_err.getError()) {
    return false;
  }

  std::unique_ptr<llvm::sampleprof::SampleProfileReader> reader =
      std::move(reader_or_err.get());
  if (reader->read() != llvm::sampleprof_error::success) {
    return false;
  }

  // LLVMProfileReader's profile symbol list will live longer than sample
  // profile reader, so need to use ProfileSymbolList::merge to copy the
  // underlying string data in reader Buffer into LLVMProfileReader's
  // profile symbol list.
  auto reader_sym_list = reader->getProfileSymbolList();
  if (reader_sym_list) {
    auto prof_sym_list =
        absl::make_unique<llvm::sampleprof::ProfileSymbolList>();
    prof_sym_list->merge(*reader_sym_list);
    SetProfileSymbolList(std::move(prof_sym_list));
  }

  for (const auto &name_profile : reader->getProfiles()) {
    SourceStack stack;
    ReadFromFunctionSamples(stack, name_profile.second);
  }
  return true;
}

void LLVMProfileReader::ReadFromFunctionSamples(
    const SourceStack &stack, const llvm::sampleprof::FunctionSamples &fs) {
  const char *func_name = GetName(fs.getName());

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
      symbol_map_->AddIndirectCallTarget(top_func_name, new_stack,
                                         GetName(target_count.getKey()),
                                         target_count.getValue());
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
