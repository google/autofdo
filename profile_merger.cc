// Merge the .afdo files.

#include <memory>
#include <string>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "gcov.h"
#if defined(HAVE_LLVM)
#include "llvm_profile_reader.h"
#include "llvm_profile_writer.h"
#endif
#include "profile_reader.h"
#include "profile_writer.h"
#include "source_info.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/base/macros.h"
#include "third_party/abseil/absl/container/node_hash_set.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/memory/memory.h"
#if defined(HAVE_LLVM)
#include "llvm/Config/llvm-config.h"
#endif
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"

ABSL_FLAG(std::string, output_file, "fbdata.afdo", "Output file name");
#if defined(HAVE_LLVM)
ABSL_FLAG(bool, is_llvm, false, "Whether the profile is for LLVM");
ABSL_FLAG(std::string, format, "binary",
          "LLVM profile format to emit. Possible values are 'text', "
          "'binary' or 'extbinary'. The binary format is a more compact "
          "representation, and the 'compactbinary' format is an even more "
          "compact representation. The 'text' format is human readable "
          "and more likely to be compatible with older versions of LLVM. "
          "The 'extbinary' format is similar to binary format but more "
          "easy to be extended. ");
ABSL_FLAG(bool, merge_special_syms, true,
          "When we merge profiles from multiple "
          "targets, we may see symbols with local linkage from different "
          "targets to have the same name. If we don't want to merge profiles "
          "for those symbols (when the option being set to false), we "
          "provide several alternatives to handle them: strip_all, "
          "keep_sole and keep_cold."
          "strip_all: strip copies from all the profiles "
          "keep_sole: keep the copy if the symbol only appears in one "
          "           profile, otherwise strip the copies. "
          "keep_cold: strip copies from all the profiles and create a cold "
          "           copy only with header but without body. ");
ABSL_FLAG(bool, include_symbol_list, false,
          "Whether to merge symbol lists and include the merged list in the "
          "output.");
ABSL_FLAG(bool, compress, false,
          "Compress all sections of the profile. The option can only be "
          "enabled when --format=extbinary. ");
ABSL_FLAG(bool, use_md5, false,
          "Use md5 names in name table. The option can only be enabled "
          "when --format=extbinary. ");
ABSL_FLAG(bool, partial_profile, false,
          "Specify the output to be a partial profile. The option can "
          "only be enabled when --format=extbinary. ");
ABSL_FLAG(bool, split_layout, false,
          "Split the profile to two parts with one part containing context "
          "sensitive information and another part not. ");
ABSL_FLAG(std::string, strip_symbols_regex, "",
          "Strip outline symbols "
          "matching the regular expression in the merged profile. ");
ABSL_FLAG(int, max_call_targets, 0,
          "Keep up specified number of the most frequently called targets at "
          "each profiled location containing call targets.");
ABSL_FLAG(int, max_inline_callsite_nesting_level, 0,
          "Limit inline callsite nesting level to specified value, and further "
          "inlined callsites are flattened.");
ABSL_FLAG(int, inline_instances_at_same_loc_cutoff, -1,
          "Cut off value to control the number of inline instances at the "
          "same location. Having too many inline instances at the same location"
          " can introduce excessive thinlto importing cost without an "
          "apparent benefit. Value < 0 has no effect.");

namespace {
// Some special symbols or symbol patterns we are going to handle.
// strip_all means for the set of symbols, we are going to strip their
// copies from all the profiles during the merge.
static const char* strip_all[] = {
    // nonexistent symbol, just for testing.
    "__strip_all_special_$erqweriwe2313541__",
    // TODO(b/145210467):
    // Strip the function below from XbinaryFDO profile. This is a workaround
    // for compilation timeout caused by too much inlining happening in the
    // large function.
    "_ZNK16GWSLogEntryProto18_"
    "InternalSerializeEPhPN6proto22io19EpsCopyOutputStreamE",
    "_ZNK20GWSLogEntryProtoLite18_"
    "InternalSerializeEPhPN6proto22io19EpsCopyOutputStreamE",
};
// keep_sole means for the set of symbols, we are going to keep the copy
// if the symbol only appears in one profile. Otherwise we are going to
// strip all its copies.
static const char* keep_sole[] = {
    // nonexistent symbol, just for testing.
    "__keep_sole_special_$sdlfa3293481293__",
};
// keep_cold means for the set of symbols, we are going to strip
// all the copies in the profiles and create a cold copy only with header
// but without body for each symbol in the set in the merged profile.
static const char* keep_cold[] = {
    "__cxx_global_var_init",
    "_GLOBAL__sub_I_",
    // nonexistent symbol, just for testing.
    "__keep_cold_special_$203128ksjldfaskf__",
};

// Verify the flags usage. Return true if the flags are used properly.
bool verifyProperFlags(bool has_prof_sym_list) {
  if (absl::GetFlag(FLAGS_format) != "extbinary") {
    if (has_prof_sym_list) {
      LOG(WARNING) << "prof_sym_list not empty, --format better be extbinary,"
                   << "otherwise prof_sym_list will be lost";
      return true;
    }
    if (absl::GetFlag(FLAGS_compress)) {
      LOG(ERROR) << "--compress can only be used if --format is extbinary";
      return false;
    }
    if (absl::GetFlag(FLAGS_use_md5)) {
      LOG(ERROR) << "--use_md5 can only be used if --format is extbinary";
      return false;
    }
    if (absl::GetFlag(FLAGS_partial_profile)) {
      LOG(ERROR) << "--partial_profile can only be used if --format is "
                 << "extbinary";
      return false;
    }
    if (absl::GetFlag(FLAGS_split_layout)) {
      LOG(ERROR) << "--split_layout can only be used if --format is "
                 << "extbinary";
      return false;
    }
  }
  return true;
}
}  // namespace
#endif

int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(argv[0]);
  std::vector<char*> positionalArguments = absl::ParseCommandLine(argc, argv);
  devtools_crosstool_autofdo::SymbolMap symbol_map;

  if (argc < 2) {
    LOG(FATAL) << "Please at least specify an input profile";
  }

#if defined(HAVE_LLVM)
  if (absl::GetFlag(FLAGS_include_symbol_list) &&
      absl::GetFlag(FLAGS_format) != "extbinary") {
    LOG(WARNING) << "--include_symbol_list is true, but symbol list is only "
                    "generated when --format=extbinary. Current format is "
                 << absl::GetFlag(FLAGS_format);
  }

  devtools_crosstool_autofdo::SpecialSyms special_syms(
      strip_all, ABSL_ARRAYSIZE(strip_all), keep_sole,
      ABSL_ARRAYSIZE(keep_sole), keep_cold, ABSL_ARRAYSIZE(keep_cold));

  absl::node_hash_set<std::string> names;

  if (!absl::GetFlag(FLAGS_is_llvm)) {
#endif
    using devtools_crosstool_autofdo::AutoFDOProfileReader;
    typedef std::unique_ptr<AutoFDOProfileReader> AutoFDOProfileReaderPtr;
    std::unique_ptr<AutoFDOProfileReaderPtr[]> readers(
        new AutoFDOProfileReaderPtr[argc - 1]);
    // TODO(dehao): merge profile reader/writer into a single class
    for (int i = 1; i < argc; i++) {
      readers[i - 1] =
          std::make_unique<AutoFDOProfileReader>(&symbol_map, true);
      readers[i - 1]->ReadFromFile(argv[i]);
    }

    symbol_map.CalculateThreshold();
    devtools_crosstool_autofdo::AutoFDOProfileWriter writer(
        &symbol_map, absl::GetFlag(FLAGS_gcov_version));
    if (!writer.WriteToFile(absl::GetFlag(FLAGS_output_file))) {
      LOG(FATAL) << "Error writing to " << absl::GetFlag(FLAGS_output_file);
    }
#if defined(HAVE_LLVM)
  } else {
    using devtools_crosstool_autofdo::LLVMProfileReader;
    using devtools_crosstool_autofdo::LLVMProfileWriter;
    typedef std::unique_ptr<LLVMProfileReader> LLVMProfileReaderPtr;

    std::unique_ptr<LLVMProfileReaderPtr[]> readers(
        new LLVMProfileReaderPtr[argc - 1]);
    llvm::sampleprof::ProfileSymbolList prof_sym_list;

#if LLVM_VERSION_MAJOR >= 12
    // Here we check if all profiles use fs-discriminators.
    int numFSDProfiles = 0;
#endif

    for (int i = 1; i < argc; i++) {
      auto reader = std::make_unique<LLVMProfileReader>(
          &symbol_map, names,
          absl::GetFlag(FLAGS_merge_special_syms) ? nullptr : &special_syms);
      CHECK(reader->ReadFromFile(argv[i])) << "when reading " << argv[i];

#if LLVM_VERSION_MAJOR >= 12
      if (reader->ProfileIsFS()) {
        numFSDProfiles++;
      }
#endif
      if (absl::GetFlag(FLAGS_include_symbol_list) &&
          absl::GetFlag(FLAGS_format) == "extbinary") {
        // Merge profile symbol list if it exists.
        llvm::sampleprof::ProfileSymbolList* input_list =
            reader->GetProfileSymbolList();
        if (input_list) prof_sym_list.merge(*input_list);
      }
      reader.reset(nullptr);
    }
    symbol_map.CalculateThreshold();
    std::unique_ptr<LLVMProfileWriter> writer(nullptr);
    if (absl::GetFlag(FLAGS_format) == "text") {
      writer.reset(new LLVMProfileWriter(llvm::sampleprof::SPF_Text));
    } else if (absl::GetFlag(FLAGS_format) == "binary") {
      writer.reset(new LLVMProfileWriter(llvm::sampleprof::SPF_Binary));
    } else if (absl::GetFlag(FLAGS_format) == "compactbinary") {
      writer = std::make_unique<LLVMProfileWriter>(
          llvm::sampleprof::SPF_Compact_Binary);
    } else if (absl::GetFlag(FLAGS_format) == "extbinary") {
      writer =
          std::make_unique<LLVMProfileWriter>(llvm::sampleprof::SPF_Ext_Binary);
    } else {
      LOG(ERROR) << "--format=" << absl::GetFlag(FLAGS_format)
                 << " is not supported. "
                 << "Use one of 'text', 'binary' or 'extbinary' format";
      return 1;
    }
    bool has_prof_sym_list = (prof_sym_list.size() > 0);
    if (!verifyProperFlags(has_prof_sym_list)) return 1;

    auto *sample_profile_writer =
        writer->CreateSampleWriter(absl::GetFlag(FLAGS_output_file));
    if (!sample_profile_writer) return 1;
#if LLVM_VERSION_MAJOR >= 12
    if (numFSDProfiles != 0 && numFSDProfiles != argc - 1) {
      LOG(WARNING) << "Merging a profile with FSDiscriminator enabled"
                   << " with a profile with FSDiscriminator disabled,"
                   << " the result profile will have FSDiscriminator enabled.";
    }
    // If resetSecLayout is needed, it should be called just after the
    // sample_profile_writer is created because resetSecLayout will reset
    // all the section flags in the extbinary profile.
    if (absl::GetFlag(FLAGS_split_layout)) {
      sample_profile_writer->resetSecLayout(llvm::sampleprof::CtxSplitLayout);
    }
    if (absl::GetFlag(FLAGS_use_fs_discriminator) || numFSDProfiles != 0) {
      devtools_crosstool_autofdo::SourceInfo::use_fs_discriminator = true;
    }
#endif
#if LLVM_VERSION_MAJOR >= 11
    if (has_prof_sym_list) {
      sample_profile_writer->setProfileSymbolList(&prof_sym_list);
    }
    if (absl::GetFlag(FLAGS_compress)) {
      sample_profile_writer->setToCompressAllSections();
    }
    if (absl::GetFlag(FLAGS_use_md5)) {
      sample_profile_writer->setUseMD5();
    }
    if (absl::GetFlag(FLAGS_partial_profile)) {
      sample_profile_writer->setPartialProfile();
    }
#endif
    // Perform optional flattening after merging to limit a CS profile to a
    // given nested level, so that the time spent on inlining using the profile
    // is bounded.
    symbol_map.FlattenNestedInlineCallsites(
        absl::GetFlag(FLAGS_max_inline_callsite_nesting_level));

    // Symbol stripping must be done after all flattening operations since they
    // can create new top level functions.
    auto strip_symbols_regex = absl::GetFlag(FLAGS_strip_symbols_regex);
    if (!strip_symbols_regex.empty()) {
      symbol_map.RemoveSymsMatchingRegex(strip_symbols_regex);
    }

    // Throw away the colder inline instances if there are too many of them
    // at the same location after profile merging. This is to control ThinLTO
    // importing cost. This is placed here so that it can be used in the
    // standalone tool as well.
    if (int max_inline_instances =
            absl::GetFlag(FLAGS_inline_instances_at_same_loc_cutoff);
        max_inline_instances > 0) {
      symbol_map.throttleInlineInstancesAtSameLocation(max_inline_instances);
    }

    // Trim call targets to specified number.
    if (int max_call_targets = absl::GetFlag(FLAGS_max_call_targets);
        max_call_targets > 0) {
      symbol_map.TrimCallTargets(max_call_targets);
    }

    writer->setSymbolMap(&symbol_map);
    if (!writer->WriteToFile(absl::GetFlag(FLAGS_output_file))) {
      LOG(FATAL) << "Error writing to " << absl::GetFlag(FLAGS_output_file);
    }
  }
#endif
  return 0;
}
