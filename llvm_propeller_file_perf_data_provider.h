#ifndef AUTOFDO_LLVM_PROPELLER_FILE_PERF_DATA_PROVIDER_H_
#define AUTOFDO_LLVM_PROPELLER_FILE_PERF_DATA_PROVIDER_H_

#if defined(HAVE_LLVM)

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_perf_data_provider.h"
#include "third_party/abseil/absl/status/statusor.h"

namespace devtools_crosstool_autofdo {

// A perf.data provider reading (by mmapping if possible) them from files.
class FilePerfDataProvider : public PerfDataProvider {
 public:
  explicit FilePerfDataProvider(std::vector<std::string> file_names)
      : file_names_(std::move(file_names)), index_(0) {}

  FilePerfDataProvider(const FilePerfDataProvider&) = delete;
  FilePerfDataProvider(FilePerfDataProvider&&) = default;
  FilePerfDataProvider& operator=(const FilePerfDataProvider&) = delete;
  FilePerfDataProvider& operator=(FilePerfDataProvider&&) = default;

  absl::StatusOr<std::optional<PerfDataProvider::BufferHandle>> GetNext()
      override;

  // Returns all perf data files upon the first call. Every next call returns an
  // empty vector.
  absl::StatusOr<std::vector<PerfDataProvider::BufferHandle>>
      GetAllAvailableOrNext() override;

 private:
  std::vector<std::string> file_names_;

  // Index into `file_names_` pointing to the file to be read by the next
  // invocation of `GetNext()`.
  int index_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // HAVE_LLVM

#endif  // AUTOFDO_LLVM_PROPELLER_FILE_PERF_DATA_PROVIDER_H_
