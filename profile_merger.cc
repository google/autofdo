// Merge the .afdo files.

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "gcov.h"
#include "llvm_profile_reader.h"
#include "llvm_profile_writer.h"
#include "profile_reader.h"
#include "profile_writer.h"
#include "symbol_map.h"
#include "third_party/abseil/absl/base/macros.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/memory/memory.h"
#include "third_party/abseil/absl/flags/parse.h"
#include "third_party/abseil/absl/flags/usage.h"
#include "llvm/Config/llvm-config.h"

ABSL_FLAG(string, output_file, "fbdata.afdo", "Output file name");
ABSL_FLAG(bool, is_llvm, false, "Whether the profile is for LLVM");
ABSL_FLAG(string, format, "binary",
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

namespace {
// Some sepcial symbols or symbol patterns we are going to handle.
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
  }
  return true;
}
}  // namespace

int main(int argc, char **argv) {
  absl::SetProgramUsageMessage(argv[0]);
  absl::ParseCommandLine(argc, argv);
  devtools_crosstool_autofdo::SymbolMap symbol_map;

  if (argc < 2) {
    LOG(FATAL) << "Please at least specify an input profile";
  }

  if (absl::GetFlag(FLAGS_include_symbol_list) &&
      absl::GetFlag(FLAGS_format) != "extbinary") {
    LOG(WARNING) << "--include_symbol_list is true, but symbol list is only "
                    "generated when --format=extbinary. Current format is "
                 << absl::GetFlag(FLAGS_format);
  }

  devtools_crosstool_autofdo::SpecialSyms special_syms(
      strip_all, ABSL_ARRAYSIZE(strip_all), keep_sole,
      ABSL_ARRAYSIZE(keep_sole), keep_cold, ABSL_ARRAYSIZE(keep_cold));

  if (!absl::GetFlag(FLAGS_is_llvm)) {
    using devtools_crosstool_autofdo::AutoFDOProfileReader;
    typedef std::unique_ptr<AutoFDOProfileReader> AutoFDOProfileReaderPtr;
    std::unique_ptr<AutoFDOProfileReaderPtr[]> readers(
        new AutoFDOProfileReaderPtr[argc - 1]);
    // TODO(dehao): merge profile reader/writer into a single class
    for (int i = 1; i < argc; i++) {
      readers[i - 1] =
          absl::make_unique<AutoFDOProfileReader>(&symbol_map, true);
      readers[i - 1]->ReadFromFile(argv[i]);
    }

    symbol_map.CalculateThreshold();
    devtools_crosstool_autofdo::AutoFDOProfileWriter writer(
        &symbol_map, absl::GetFlag(FLAGS_gcov_version));
    if (!writer.WriteToFile(absl::GetFlag(FLAGS_output_file))) {
      LOG(FATAL) << "Error writing to " << absl::GetFlag(FLAGS_output_file);
    }
  } else {
    using devtools_crosstool_autofdo::LLVMProfileReader;
    using devtools_crosstool_autofdo::LLVMProfileWriter;
    typedef std::unique_ptr<LLVMProfileReader> LLVMProfileReaderPtr;

    std::unique_ptr<LLVMProfileReaderPtr[]> readers(
        new LLVMProfileReaderPtr[argc - 1]);
    llvm::sampleprof::ProfileSymbolList prof_sym_list;

    for (int i = 1; i < argc; i++) {
      readers[i - 1] = absl::make_unique<LLVMProfileReader>(
          &symbol_map,
          absl::GetFlag(FLAGS_merge_special_syms) ? nullptr : &special_syms);
      readers[i - 1]->ReadFromFile(argv[i]);

      if (absl::GetFlag(FLAGS_include_symbol_list) &&
          absl::GetFlag(FLAGS_format) == "extbinary") {
        // Merge profile symbol list if it exists.
        llvm::sampleprof::ProfileSymbolList* input_list =
            readers[i - 1]->GetProfileSymbolList();
        if (input_list) prof_sym_list.merge(*input_list);
      }
    }
    symbol_map.CalculateThreshold();
    std::unique_ptr<LLVMProfileWriter> writer(nullptr);
    if (absl::GetFlag(FLAGS_format) == "text") {
      writer.reset(new LLVMProfileWriter(llvm::sampleprof::SPF_Text));
    } else if (absl::GetFlag(FLAGS_format) == "binary") {
      writer.reset(new LLVMProfileWriter(llvm::sampleprof::SPF_Binary));
    } else if (absl::GetFlag(FLAGS_format) == "compactbinary") {
      writer = absl::make_unique<LLVMProfileWriter>(
          llvm::sampleprof::SPF_Compact_Binary);
    } else if (absl::GetFlag(FLAGS_format) == "extbinary") {
      writer = absl::make_unique<LLVMProfileWriter>(
          llvm::sampleprof::SPF_Ext_Binary);
    } else {
      LOG(ERROR) << "--format=" << absl::GetFlag(FLAGS_format)
                 << " is not supported. "
                 << "Use one of 'text', 'binary' or 'extbinary' format";
      return 1;
    }
    bool has_prof_sym_list = (prof_sym_list.size() > 0);
    if (!verifyProperFlags(has_prof_sym_list)) return 1;

    auto sample_profile_writer =
        writer->CreateSampleWriter(absl::GetFlag(FLAGS_output_file));
    if (!sample_profile_writer) return 1;

    writer->setSymbolMap(&symbol_map);
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
    writer->setSymbolMap(&symbol_map);
    if (!writer->WriteToFile(absl::GetFlag(FLAGS_output_file))) {
      LOG(FATAL) << "Error writing to " << absl::GetFlag(FLAGS_output_file);
    }
  }
  return 0;
}
