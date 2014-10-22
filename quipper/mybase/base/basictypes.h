// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_MYBASE_BASE_BASICTYPES_H_
#define CHROMIUMOS_WIDE_PROFILING_MYBASE_BASE_BASICTYPES_H_

#include <limits>

#include "base/macros.h"

static const uint64_t kuint64max = std::numeric_limits<uint64_t>::max();

#define arraysize(x) (sizeof(x) / sizeof(*x))

#endif  // CHROMIUMOS_WIDE_PROFILING_MYBASE_BASE_BASICTYPES_H_
