#include "llvm_propeller_file_perf_data_provider.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_perf_data_provider.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "status_macros.h"

namespace devtools_crosstool_autofdo {

absl::StatusOr<std::optional<PerfDataProvider::BufferHandle>>
FilePerfDataProvider::GetNext() {
  if (index_ >= file_names_.size()) return std::nullopt;

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> perf_file_content =
      llvm::MemoryBuffer::getFile(file_names_[index_], /*IsText=*/false,
                                  /*RequiresNullTerminator=*/false,
                                  /*IsVolatile=*/false);
  if (!perf_file_content) {
    return absl::InternalError(
        absl::StrCat("Error in llvm::MemoryBuffer::getFile: ",
                     perf_file_content.getError().message()));
  }

  std::string description = absl::StrFormat(
      "[%d/%d] %s", index_ + 1, file_names_.size(), file_names_[index_]);
  ++index_;
  return BufferHandle{.description = std::move(description),
                      .buffer = std::move(*perf_file_content)};
}

absl::StatusOr<std::vector<PerfDataProvider::BufferHandle>>
FilePerfDataProvider::GetAllAvailableOrNext() {
  std::vector<PerfDataProvider::BufferHandle> result;
  while (true) {
    ASSIGN_OR_RETURN(std::optional<PerfDataProvider::BufferHandle> next,
                     GetNext());
    if (!next.has_value()) return result;
    result.push_back(std::move(next.value()));
  }
  return result;
}

}  // namespace devtools_crosstool_autofdo
