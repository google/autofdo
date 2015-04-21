// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_LIMITS_H_
#define CHROMIUMOS_WIDE_PROFILING_LIMITS_H_

#include <limits>

namespace quipper {

static constexpr uint32_t kUint32Max = std::numeric_limits<uint32_t>::max();
static constexpr uint64_t kUint64Max = std::numeric_limits<uint64_t>::max();

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_LIMITS_H_
