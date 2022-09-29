#ifndef AUTOFDO_LLVM_PROPELLER_PERF_DATA_PROVIDER_H_
#define AUTOFDO_LLVM_PROPELLER_PERF_DATA_PROVIDER_H_

#if defined(HAVE_LLVM)

#include <memory>
#include <optional>
#include <string>

#include "third_party/abseil/absl/status/statusor.h"
#include "llvm/Support/MemoryBuffer.h"

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
  };

  virtual ~PerfDataProvider() = default;

  // Returns the next perf data file, represented as an llvm::MemoryBuffer,
  // so that file-based providers can mmap the file instead. If there are no
  // more perf data files to be processed, returns `std::nullopt`.
  virtual absl::StatusOr<std::optional<BufferHandle>> GetNext() = 0;
};

}  // namespace devtools_crosstool_autofdo

#endif  // HAVE_LLVM

#endif  // AUTOFDO_LLVM_PROPELLER_PERF_DATA_PROVIDER_H_
