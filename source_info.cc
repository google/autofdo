// Class to represent the source info.

#include "base/logging.h"
#include "source_info.h"

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
  ret = file_name.compare(p.file_name);
  if (ret != 0) {
    return ret < 0;
  }
  return dir_name.compare(p.dir_name) < 0;
}
}  // namespace devtools_crosstool_autofdo
