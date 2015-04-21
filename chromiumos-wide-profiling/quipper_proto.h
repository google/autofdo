// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_QUIPPER_PROTO_H_
#define CHROMIUMOS_WIDE_PROFILING_QUIPPER_PROTO_H_

#include <google/protobuf/text_format.h>

#include "perf_data.pb.h"  // NOLINT(build/include)

namespace quipper {

using ::google::protobuf::RepeatedPtrField;
using ::google::protobuf::TextFormat;

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_QUIPPER_PROTO_H_
