// Class to represent the source info.

#include "source_info.h"

namespace devtools_crosstool_autofdo {

#if defined(HAVE_LLVM)
bool SourceInfo::use_fs_discriminator = false;
bool SourceInfo::use_base_only_in_fs_discriminator = false;
#endif

}  // namespace devtools_crosstool_autofdo
