#include "llvm_propeller_binary_content.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "addr2cu.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/log/log.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"

namespace {
// Convert binary data stored in data[...] into text representation.
std::string BinaryDataToAscii(absl::string_view data) {
  std::string ascii(data.size() * 2, 0);
  const char heximal[] = "0123456789abcdef";
  for (int i = 0; i < data.size(); ++i) {
    uint8_t d(data[i]);
    ascii[i * 2] = heximal[((d >> 4) & 0xf)];
    ascii[i * 2 + 1] = heximal[(d & 0xf)];
  }
  return ascii;
}
}  // namespace

namespace devtools_crosstool_autofdo {

// TODO(shenhan): remove the following code once it is upstreamed.
template <class ELFT>
std::string ELFFileUtil<ELFT>::GetBuildId() {
  if (!elf_file_) return "";
  auto hex_to_char = [](uint8_t v) -> char {
    if (v < 10) return '0' + v;
    return 'a' + (v - 10);
  };
  std::vector<std::string> build_ids;
  for (const typename ELFT::Shdr &shdr :
       llvm::cantFail(elf_file_->sections())) {
    llvm::Expected<llvm::StringRef> section_name =
        elf_file_->getSectionName(shdr);
    if (!section_name || shdr.sh_type != llvm::ELF::SHT_NOTE ||
        (*section_name != kBuildIDSectionName &&
         *section_name != kKernelBuildIDSectionName))
      continue;
    llvm::Error err = llvm::Error::success();
    for (const typename ELFT::Note &note : elf_file_->notes(shdr, err)) {
      llvm::StringRef r = note.getName();
      if (r == kBuildIdNoteName) {
        llvm::ArrayRef<uint8_t> build_id = note.getDesc(shdr.sh_addralign);
        std::string build_id_str(build_id.size() * 2, '0');
        int k = 0;
        for (uint8_t t : build_id) {
          build_id_str[k++] = hex_to_char((t >> 4) & 0xf);
          build_id_str[k++] = hex_to_char(t & 0xf);
        }
        build_ids.push_back(std::move(build_id_str));
      }
    }
    if (errorToBool(std::move(err)))
      LOG(WARNING) << "error happened iterating note entries in '"
                   << section_name->str() << "'";
  }
  if (build_ids.empty()) return "";
  if (build_ids.size() > 1) {
    LOG(WARNING) << "more than 1 build id entries found in the binary, only "
                    "the first one will be returned";
  }
  return build_ids.front();
}

template <class ELFT>
bool ELFFileUtil<ELFT>::ReadLoadableSegments(BinaryContent &binary_content) {
  if (!elf_file_) return false;
  auto program_headers = elf_file_->program_headers();
  if (!program_headers) return false;

  for (const typename ELFT::Phdr &phdr : *program_headers) {
    if (phdr.p_type != llvm::ELF::PT_LOAD ||
        ((phdr.p_flags & llvm::ELF::PF_X) == 0))
      continue;

    binary_content.segments.push_back(
        {phdr.p_offset, phdr.p_vaddr, phdr.p_memsz});
  }
  if (binary_content.segments.empty()) {
    LOG(ERROR) << "No loadable and executable segments found in '"
               << binary_content.file_name << "'.";
    return false;
  }
  return true;
}

std::unique_ptr<ELFFileUtilBase> CreateELFFileUtil(
    llvm::object::ObjectFile *object_file) {
  if (!object_file) return nullptr;
  llvm::StringRef content = object_file->getData();
  const char *elf_start = content.data();

  if (content.size() <= strlen(llvm::ELF::ElfMagic) ||
      strncmp(elf_start, llvm::ELF::ElfMagic, strlen(llvm::ELF::ElfMagic))) {
    LOG(ERROR) << "Not a valid ELF file.";
    return nullptr;
  }
  const char elf_class = elf_start[llvm::ELF::EI_CLASS];
  const char elf_data = elf_start[llvm::ELF::EI_DATA];
  if (elf_class == llvm::ELF::ELFCLASS32 &&
      elf_data == llvm::ELF::ELFDATA2LSB) {
    return std::make_unique<ELFFileUtil<llvm::object::ELF32LE>>(object_file);
  } else if (elf_class == llvm::ELF::ELFCLASS32 &&
             elf_data == llvm::ELF::ELFDATA2MSB) {
    return std::make_unique<ELFFileUtil<llvm::object::ELF32BE>>(object_file);
  } else if (elf_class == llvm::ELF::ELFCLASS64 &&
             elf_data == llvm::ELF::ELFDATA2LSB) {
    return std::make_unique<ELFFileUtil<llvm::object::ELF64LE>>(object_file);
  } else if (elf_class == llvm::ELF::ELFCLASS64 &&
             elf_data == llvm::ELF::ELFDATA2MSB) {
    return std::make_unique<ELFFileUtil<llvm::object::ELF64BE>>(object_file);
  }
  LOG(ERROR) << "Unrecognized ELF file data.";
  return nullptr;
}

// Initializes BinaryContent object:
//  - setup file content memory buffer
//  - setup object file pointer
//  - setup "PIE" bit
//  - read loadable and executable segments
absl::StatusOr<std::unique_ptr<BinaryContent>> GetBinaryContent(
    absl::string_view binary_file_name) {
  auto binary_content = std::make_unique<BinaryContent>();
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> file =
      llvm::MemoryBuffer::getFile(binary_file_name);
  if (!file) {
    return absl::FailedPreconditionError(
        absl::StrCat("Failed to read file '", binary_file_name,
                     "': ", file.getError().message()));
  }
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> obj =
      llvm::object::ObjectFile::createELFObjectFile(
          llvm::MemoryBufferRef(*(*file)));
  if (!obj) {
    std::string error_message;
    llvm::raw_string_ostream raw_string_ostream(error_message);
    raw_string_ostream << obj.takeError();
    return absl::FailedPreconditionError(
        absl::StrCat("Not a valid ELF file '", binary_file_name,
                     "': ", raw_string_ostream.str()));
  }
  llvm::object::ELFObjectFileBase *elf_obj =
      llvm::dyn_cast<llvm::object::ELFObjectFileBase, llvm::object::ObjectFile>(
          (*obj).get());
  if (!elf_obj) {
    return absl::FailedPreconditionError(
        absl::StrCat("Not a valid ELF file '", binary_file_name, "."));
  }
  binary_content->file_name = binary_file_name;
  binary_content->file_content = std::move(*file);
  binary_content->object_file = std::move(*obj);

  std::string dwp_file = absl::StrCat(binary_content->file_name, ".dwp");
  if (!llvm::sys::fs::exists(dwp_file)) dwp_file = "";
  binary_content->dwp_file_name = dwp_file;
  absl::StatusOr<std::unique_ptr<llvm::DWARFContext>> dwarf_context =
      CreateDWARFContext(*binary_content->object_file, dwp_file);
  if (dwarf_context.ok()) {
    binary_content->dwarf_context = std::move(*dwarf_context);
  }else {
    LOG(WARNING) << "Failed to create DWARF context: " << dwarf_context.status()
                 << "\nNo module names wil be available";
  }

  binary_content->is_pie = (elf_obj->getEType() == llvm::ELF::ET_DYN);
  LOG(INFO) << "'" << binary_file_name
            << "' is PIE: " << binary_content->is_pie;

  std::unique_ptr<ELFFileUtilBase> elf_file_util =
      CreateELFFileUtil(binary_content->object_file.get());
  CHECK_NE(elf_file_util.get(), nullptr);
  binary_content->build_id = elf_file_util->GetBuildId();
  if (!binary_content->build_id.empty())
    LOG(INFO) << "Build Id found in '" << binary_file_name
              << "': " << binary_content->build_id;

  if (!elf_file_util->ReadLoadableSegments(*binary_content))
    return absl::InternalError("Unable to read loadable segments");
  return binary_content;
}

}  // namespace devtools_crosstool_autofdo
