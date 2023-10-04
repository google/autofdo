#ifndef AUTOFDO_LLVM_PROPELLER_BINARY_CONTENT_H_
#define AUTOFDO_LLVM_PROPELLER_BINARY_CONTENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"

namespace devtools_crosstool_autofdo {

// BinaryContent represents information for an ELF executable or a shared
// object, the data contained include (loadable) segments, file name, file
// content and DYN tag (is_pie).
struct BinaryContent {
  struct Segment {
    uint64_t offset;
    uint64_t vaddr;
    uint64_t memsz;
  };

  std::string file_name;
  // If not empty, it is the existing dwp file for the binary.
  std::string dwp_file_name;
  std::unique_ptr<llvm::MemoryBuffer> file_content = nullptr;
  std::unique_ptr<llvm::object::ObjectFile> object_file = nullptr;
  std::unique_ptr<llvm::DWARFContext> dwarf_context = nullptr;
  bool is_pie = false;
  std::vector<Segment> segments;
  std::string build_id;
};

// Utility class that wraps utility functions that need templated
// ELFFile<ELFT> support.
class ELFFileUtilBase {
 protected:
  ELFFileUtilBase() {}

 public:
  virtual ~ELFFileUtilBase() = default;

  virtual std::string GetBuildId() = 0;

  virtual bool ReadLoadableSegments(BinaryContent &binary_content) = 0;

 protected:
  static constexpr llvm::StringRef kBuildIDSectionName = ".note.gnu.build-id";
  // Kernel images built via gbuild use section name ".notes" for buildid.
  static constexpr llvm::StringRef kKernelBuildIDSectionName = ".notes";
  static constexpr llvm::StringRef kBuildIdNoteName = "GNU";

  friend std::unique_ptr<ELFFileUtilBase> CreateELFFileUtil(
      llvm::object::ObjectFile *object_file);
};

template <class ELFT>
class ELFFileUtil : public ELFFileUtilBase {
 public:
  explicit ELFFileUtil(llvm::object::ObjectFile *object) {
    llvm::object::ELFObjectFile<ELFT> *elf_object =
        llvm::dyn_cast<llvm::object::ELFObjectFile<ELFT>,
                       llvm::object::ObjectFile>(object);
    if (elf_object) elf_file_ = &elf_object->getELFFile();
  }

  // Get binary build id.
  std::string GetBuildId() override;

  // Read loadable and executable segment information into
  // BinaryContent::segments.
  bool ReadLoadableSegments(BinaryContent &binary_content) override;

 private:
  const llvm::object::ELFFile<ELFT> *elf_file_ = nullptr;
};

std::unique_ptr<ELFFileUtilBase> CreateELFFileUtil(
    llvm::object::ObjectFile *object_file);

absl::StatusOr<std::unique_ptr<BinaryContent>> GetBinaryContent(
    absl::string_view binary_file_name);

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_BINARY_CONTENT_H_
