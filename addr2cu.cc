#include "addr2cu.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/log/check.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/Object/ObjectFile.h"

namespace devtools_crosstool_autofdo {

absl::StatusOr<std::unique_ptr<llvm::DWARFContext>> CreateDWARFContext(
    const llvm::object::ObjectFile &obj, absl::string_view dwp_file) {
  std::unique_ptr<llvm::DWARFContext> dwarf_context =
      llvm::DWARFContext::create(
          obj, llvm::DWARFContext::ProcessDebugRelocations::Process,
          /*const LoadedObjectInfo *L=*/ nullptr, std::string(dwp_file));
  CHECK_NE(dwarf_context.get(), nullptr);
  if (dwp_file.empty() &&
      absl::c_any_of(dwarf_context->compile_units(),
                   [](std::unique_ptr<llvm::DWARFUnit> &cu) {
                     return cu->getUnitDIE().getTag() ==
                            llvm::dwarf::DW_TAG_skeleton_unit;
                   })) {
    return absl::FailedPreconditionError(
        "skeleton unit found without a corresponding dwp file");
  }
  if (!dwarf_context->getNumCompileUnits()) {
    return absl::FailedPreconditionError(
        "no compilation unit found, binary must be built with debuginfo");
  }
  return dwarf_context;
}

absl::StatusOr<absl::string_view> Addr2Cu::GetCompileUnitFileNameForCodeAddress(
    uint64_t code_address) {
  llvm::DWARFCompileUnit *unit =
      dwarf_context_.getCompileUnitForCodeAddress(code_address);
  if (unit == nullptr) {
    return absl::FailedPreconditionError(absl::StrFormat(
        "no compile unit found on address 0x%x", code_address));
  }
  llvm::DWARFDie die = unit->getNonSkeletonUnitDIE();
  std::optional<llvm::DWARFFormValue> form_value =
      die.findRecursively({llvm::dwarf::DW_AT_name});
  llvm::StringRef name = llvm::dwarf::toStringRef(form_value, "");
  return absl::string_view(name.data(), name.size());
}
}  // namespace devtools_crosstool_autofdo
