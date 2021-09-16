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

#if defined(HAVE_LLVM)
  SourceInfo(const char *func_name, llvm::StringRef dir_name,
             llvm::StringRef file_name, uint32_t start_line, uint32_t line,
             uint32_t discriminator)
#else
  SourceInfo(const char *func_name, std::string dir_name, std::string file_name,
             uint32_t start_line, uint32_t line, uint32_t discriminator)
#endif
      : func_name(func_name),
        dir_name(dir_name),
        file_name(file_name),
        start_line(start_line),
        line(line),
        discriminator(discriminator) {
  }

  bool operator<(const SourceInfo &p) const;

  std::string RelativePath() const {
    if (!dir_name.empty())
      return dir_name + "/" + file_name;
    if (!file_name.empty()) return file_name;
    return std::string();
  }

  uint64_t Offset(bool use_discriminator_encoding) const {
#if defined(HAVE_LLVM)
    return GenerateOffset(
        line - start_line,
        (use_discriminator_encoding && !use_fs_discriminator
             ? llvm::DILocation::getBaseDiscriminatorFromDiscriminator(
                   discriminator)
             : discriminator));
#else
    return ((line - start_line) << 16) | discriminator;
#endif
  }

  uint32_t DuplicationFactor() const {
#if defined(HAVE_LLVM)
    if (use_fs_discriminator) return 1;
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

  // Return the offset using the line number and the discriminator.
  static constexpr uint64_t GenerateOffset(uint32_t Line,
                                           uint32_t Discriminator) {
    return (static_cast<uint64_t>(Line) << 32) |
           static_cast<uint64_t>(Discriminator);
  }

  // Return the compressed offset value (from uint64_t to uint32_t).
  static constexpr uint32_t GenerateCompressedOffset(uint64_t Offset) {
    return static_cast<uint32_t>((Offset >> 16) | (Offset & 0xffff));
  }

  // Return the line number from the offset.
  static constexpr uint32_t GetLineNumberFromOffset(uint64_t Offset) {
    return (Offset >> 32) & 0xffff;
  }

  // Return the discrminator from the offset.
  static constexpr uint32_t GetDiscriminatorFromOffset(uint64_t Offset) {
    return Offset & 0xffffffff;
  }

#if defined(HAVE_LLVM)
  // If the discriminators are of fs-discriminator format.
  static bool use_fs_discriminator;
#endif

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
