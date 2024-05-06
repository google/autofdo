#ifndef AUTOFDOLLVM_PROPELLER_FILE_PERF_DATA_PROVIDER_H_
#define AUTOFDOLLVM_PROPELLER_FILE_PERF_DATA_PROVIDER_H_

#include "third_party/abseil/absl/base/nullability.h"
#if defined(HAVE_LLVM)

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_perf_data_provider.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "base/status_macros.h"

namespace devtools_crosstool_autofdo {

// File reader interface used by `FilePerfDataProvider`.
class FileReader {
 public:
  virtual ~FileReader() = default;

  absl::Status stat;
  
  // Reads and returns the content of the file specified with the path
  // `file_name`.
  virtual absl::StatusOr<std::unique_ptr<llvm::MemoryBuffer>> ReadFile(
      absl::string_view file_name) = 0;
};

// Generic file reader using LLVM MemoryBuffer API.
class GenericFileReader : public FileReader {
 public:
  GenericFileReader() = default;
  GenericFileReader(const GenericFileReader&) = delete;
  GenericFileReader(GenericFileReader&&) = default;
  GenericFileReader& operator=(const GenericFileReader&) = delete;
  GenericFileReader& operator=(GenericFileReader&&) = default;

  absl::StatusOr<std::unique_ptr<llvm::MemoryBuffer>> ReadFile(
      absl::string_view file_name) override {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> perf_file_content =
        llvm::MemoryBuffer::getFile(file_name, /*IsText=*/false,
                                    /*RequiresNullTerminator=*/false,
                                    /*IsVolatile=*/false);
    if (!perf_file_content) {
      return absl::InternalError(
          absl::StrCat(perf_file_content.getError().message(),
                       "; When reading file ", file_name));
    }
    return std::move(perf_file_content.get());
  }
};

// A perf.data provider interface for reading from files.
class FilePerfDataProvider : public PerfDataProvider {
 public:
  FilePerfDataProvider(absl::Nonnull<std::unique_ptr<FileReader>> file_reader,
                       std::vector<std::string> file_names)
      : file_reader_(std::move(file_reader)),
        file_names_(std::move(file_names)),
        index_(0) {}

  FilePerfDataProvider(const FilePerfDataProvider&) = delete;
  FilePerfDataProvider(FilePerfDataProvider&&) = default;
  FilePerfDataProvider& operator=(const FilePerfDataProvider&) = delete;
  FilePerfDataProvider& operator=(FilePerfDataProvider&&) = default;

  absl::StatusOr<std::optional<PerfDataProvider::BufferHandle>> GetNext()
      override;

  // Returns all perf data files upon the first call. Every next call returns
  // an empty vector.
  absl::StatusOr<std::vector<PerfDataProvider::BufferHandle>>
  GetAllAvailableOrNext() override {
    std::vector<PerfDataProvider::BufferHandle> result;
    while (true) {
      ASSIGN_OR_RETURN(std::optional<PerfDataProvider::BufferHandle> next,
                       GetNext());
      if (!next.has_value()) return result;
      result.push_back(std::move(next.value()));
    }
    return result;
  }

 private:
  std::unique_ptr<FileReader> file_reader_;
  std::vector<std::string> file_names_;

  // Index into `file_names_` pointing to the file to be read by the next
  // invocation of `GetNext()`.
  int index_;
};

// Generic perf.data file provider using LLVM MemoryBuffer API.
class GenericFilePerfDataProvider : public FilePerfDataProvider {
 public:
  explicit GenericFilePerfDataProvider(std::vector<std::string> file_names)
      : FilePerfDataProvider(std::make_unique<GenericFileReader>(),
                             std::move(file_names)) {}
  GenericFilePerfDataProvider(const GenericFilePerfDataProvider&) = delete;
  GenericFilePerfDataProvider(GenericFilePerfDataProvider&&) = default;
  GenericFilePerfDataProvider& operator=(const GenericFilePerfDataProvider&) =
      delete;
  GenericFilePerfDataProvider& operator=(GenericFilePerfDataProvider&&) =
      default;
};

}  // namespace devtools_crosstool_autofdo

#endif  // HAVE_LLVM

#endif  // AUTOFDOLLVM_PROPELLER_FILE_PERF_DATA_PROVIDER_H_
