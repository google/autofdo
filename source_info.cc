// Class to represent the source info.

#include "source_info.h"

#include "base/logging.h"
#include "llvm/ADT/StringRef.h"

namespace {
int StrcmpMaybeNull(const char *a, const char *b) {
  if (a == nullptr) {
    a = "";
  }
  if (b == nullptr) {
    b = "";
  }
  return strcmp(a, b);
}
}  // namespace

namespace devtools_crosstool_autofdo {
bool SourceInfo::operator<(const SourceInfo &p) const {
  if (line != p.line) {
    return line < p.line;
  }
  if (start_line != p.start_line) {
    return start_line < p.start_line;
  }
  if (discriminator != p.discriminator) {
    return discriminator < p.discriminator;
  }
  int ret = StrcmpMaybeNull(func_name, p.func_name);
  if (ret != 0) {
    return ret < 0;
  }
  // The comparison below relies on the lexicographical behavior of
  // llvm::StringRef::compare. C++ string compare should return the same result,
  // but since converting to llvm::StringRef is very cheap, and both functions
  // may not stay in sync, preferring to retain the prior functionality.
  auto p_file_name_ref = llvm::StringRef(p.file_name);
  auto file_name_ref = llvm::StringRef(file_name);
  ret = file_name_ref.compare(p_file_name_ref);
  if (ret != 0) {
    return ret < 0;
  }
  auto p_dir_name_ref = llvm::StringRef(p.dir_name);
  auto dir_name_ref = llvm::StringRef(dir_name);
  return dir_name_ref.compare(p_dir_name_ref) < 0;
}
}  // namespace devtools_crosstool_autofdo
