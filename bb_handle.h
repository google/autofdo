#ifndef AUTOFDO_BB_HANDLE_H_
#define AUTOFDO_BB_HANDLE_H_

#include <optional>

#include "third_party/abseil/absl/strings/str_format.h"

namespace devtools_crosstool_autofdo {

// A struct representing one basic block entry in the BB address map.
struct BbHandle {
  int function_index, bb_index;

  bool operator==(const BbHandle &other) const {
    return function_index == other.function_index && bb_index == other.bb_index;
  }

  bool operator!=(const BbHandle &other) const { return !(*this == other); }

  template <typename H>
  friend H AbslHashValue(H h, const BbHandle &bb_handle) {
    return H::combine(std::move(h), bb_handle.function_index,
                      bb_handle.bb_index);
  }

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const BbHandle &bb_handle) {
    absl::Format(&sink, "%d#%d", bb_handle.function_index, bb_handle.bb_index);
  }

  template <typename Sink>
  friend void AbslStringify(Sink &sink,
                            const std::optional<BbHandle> &bb_handle) {
    if (bb_handle.has_value()) {
      absl::Format(&sink, "%v", *bb_handle);
    } else {
      absl::Format(&sink, "%s", "unknown");
    }
  }
};

// This struct captures the call and return information about a single callsite.
// Specifically, the function that is called and the basic block which returns
// back to that callsite. Note that the return block may be in a different
// function than the callee (which may happen if the callee has a tail call
// itself).
struct CallRetInfo {
  // Index of the callee function (or `std::nullopt` if unknown).
  std::optional<int> callee;
  // Return block (or `std::nullopt` if unknown).
  std::optional<BbHandle> return_bb;

  template <typename H>
  friend H AbslHashValue(H h, const CallRetInfo &call_ret) {
    return H::combine(std::move(h), call_ret.callee, call_ret.return_bb);
  }

  bool operator==(const CallRetInfo &other) const {
    return callee == other.callee && return_bb == other.return_bb;
  }

  bool operator!=(const CallRetInfo &other) const { return !(*this == other); }

  template <typename Sink>
  friend void AbslStringify(Sink &sink, const CallRetInfo &call_ret) {
    absl::Format(&sink, "call:");
    if (call_ret.callee.has_value())
      absl::Format(&sink, "%d", call_ret.callee.value());
    else
      absl::Format(&sink, "unknown");
    absl::Format(&sink, "#ret:%v", call_ret.return_bb);
  }
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_BB_HANDLE_H_
