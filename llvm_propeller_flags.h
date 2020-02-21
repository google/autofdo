#include "config.h"
#if defined(HAVE_LLVM)
#ifndef _LLVM_PROPELLER_FLAGS_H_
#define _LLVM_PROPELLER_FLAGS_H_

#include "gflags/gflags.h"

DECLARE_bool(dot_number_encoding);
DECLARE_string(match_mmap_file);
DECLARE_bool(ignore_build_id);
DECLARE_bool(gen_path_profile);

#endif
#endif

