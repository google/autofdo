#ifndef AUTOFDO_LLVM_PROPELLER_PERF_DATA_PROVIDER_H_
#define AUTOFDO_LLVM_PROPELLER_PERF_DATA_PROVIDER_H_

#if defined(HAVE_LLVM)

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "third_party/abseil/absl/status/statusor.h"
#include "llvm/Support/MemoryBuffer.h"
#include "base/status_macros.h"

namespace devtools_crosstool_autofdo {

// Interface for raw perf data providers. They may read preexisting perf data
// from files or collect them on the fly from currently running binaries.
class PerfDataProvider {
 public:
  // Handle to a potentially named memory buffer.
  struct BufferHandle {
    // The description of this buffer if available, e.g. the file name for
    // file-based buffers, or a description of when and where the profile has
    // been collected from for on-the-fly profiling. Can be used for debug
    // logging, but there is no guarantee of any particular format of this field
    // and especially no guarantee that the format will not change.
    std::string description;
    // Buffer containing the perf.data file.
    std::unique_ptr<llvm::MemoryBuffer> buffer;

    template <typename Sink>
    friend void AbslStringify(Sink& sink, const BufferHandle& handle) {
      absl::Format(&sink, "[%s]", handle.description);
    }
  };

  virtual ~PerfDataProvider() = default;

  // Returns the next perf data file, represented as an llvm::MemoryBuffer,
  // so that file-based providers can mmap the file instead. If there are no
  // more perf data files to be processed, returns `std::nullopt`.
  virtual absl::StatusOr<std::optional<BufferHandle>> GetNext() = 0;

  // Returns all perf data currently available, or the next perf data file if
  // there is none available. If there are no more perf data to be processed,
  // returns an empty vector. The base implementation assumes there are no
  // perf data available and calls `GetNext()` to get the next profile.
  virtual absl::StatusOr<std::vector<PerfDataProvider::BufferHandle>>
  GetAllAvailableOrNext() {
    std::vector<PerfDataProvider::BufferHandle> result = {};
    ASSIGN_OR_RETURN(std::optional<BufferHandle> next, GetNext());
    // If no more profiles, return the empty vector.
    if (!next.has_value()) return result;
    result.push_back(std::move(*next));
    return result;
  }
};

}  // namespace devtools_crosstool_autofdo

#endif  // HAVE_LLVM

#endif  // AUTOFDO_LLVM_PROPELLER_PERF_DATA_PROVIDER_H_
