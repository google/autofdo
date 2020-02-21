#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_propeller_flags.h"

DEFINE_bool(dot_number_encoding, false, "Use dot number encoding for labels.");
DEFINE_string(match_mmap_file, "", "Match mmap event file path.");
DEFINE_bool(ignore_build_id, false, "Ignore build id match.");
DEFINE_bool(gen_path_profile, false, "Generate path profile.");

#endif
