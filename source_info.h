// Class to represent the source info.

#ifndef AUTOFDO_SOURCE_INFO_H_
#define AUTOFDO_SOURCE_INFO_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/integral_types.h"
#include "base/macros.h"
#if defined(HAVE_LLVM)
#include "llvm/IR/DebugInfoMetadata.h"
#endif

namespace devtools_crosstool_autofdo {

// Represents the source position.
struct SourceInfo {
  SourceInfo() : func_name(NULL), start_line(0), line(0), discriminator(0) {}

  SourceInfo(const char *func_name, std::string dir_name,
             std::string file_name, uint32_t start_line, uint32_t line,
             uint32_t discriminator)
      : func_name(func_name),
        dir_name(dir_name),
        file_name(file_name),
        start_line(start_line),
        line(line),
        discriminator(discriminator) {}

  bool operator<(const SourceInfo &p) const;

  std::string RelativePath() const {
    if (!dir_name.empty())
      return dir_name + "/" + file_name;
    if (!file_name.empty()) return file_name;
    return std::string();
  }

  uint32_t Offset(bool use_discriminator_encoding) const {
#if defined(HAVE_LLVM)
    return ((line - start_line) << 16) |
           (use_discriminator_encoding
                ? llvm::DILocation::getBaseDiscriminatorFromDiscriminator(
                      discriminator)
                : discriminator);
#else
    return ((line - start_line) << 16) | discriminator;
#endif
  }

  uint32_t DuplicationFactor() const {
#if defined(HAVE_LLVM)
    return llvm::DILocation::getDuplicationFactorFromDiscriminator(
        discriminator);
#else
    return 1;
#endif
  }

  bool HasInvalidInfo() const {
    if (start_line == 0 || line == 0) return true;
    return false;
  }

  const char *func_name;
  std::string dir_name;
  std::string file_name;
  uint32_t start_line;
  uint32_t line;
  uint32_t discriminator;
};

typedef std::vector<SourceInfo> SourceStack;
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_SOURCE_INFO_H_
