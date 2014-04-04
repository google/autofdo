// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_BASICTYPES_H_
#define BASE_BASICTYPES_H_

#include <stdint.h>

#include "base/macros.h"

typedef uint64_t uint64;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;

static const uint64 kuint64max = ((uint64) 0xFFFFFFFFFFFFFFFFLL);

#define arraysize(x) (sizeof(x) / sizeof(*x))

#endif  // BASE_BASICTYPES_H_
