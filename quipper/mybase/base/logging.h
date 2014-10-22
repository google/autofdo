// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_MYBASE_BASE_LOGGING_H_
#define CHROMIUMOS_WIDE_PROFILING_MYBASE_BASE_LOGGING_H_

#include <stdint.h>

#include <iostream>  // NOLINT(readability/streams)
#include <sstream>

#include "base/macros.h"

// Emulate Chrome-like logging.

// LogLevel is an enumeration that holds the log levels like libbase does.
enum LogLevel {
  FATAL,
  ERROR,
  INFO,
  WARNING,
};

// Class that emulates logging from libbase. It prints to std::cerr. It features
// an explicit constructor that must be used to specify the level of the log.
class LOG {
 public:
  explicit LOG(int level) {
    level_ = level;
  }

  ~LOG() {
    std::cerr << ss_.str() << std::endl;
  }

  template <class T> LOG& operator <<(const T& x) {
    ss_ << x;
    return *this;
  }

 private:
  LOG() {}
  std::ostringstream ss_;
  int level_;
};

// Some macros from libbase that we use.
#define CHECK(x) if (!(x)) LOG(FATAL) << #x
#define CHECK_GT(x, y) if (!(x > y)) LOG(FATAL) << #x << " > " << #y << "failed"
#define CHECK_GE(x, y) if (!(x >= y)) LOG(FATAL) << #x << " >= " << #y \
                                                 << "failed"
#define CHECK_LE(x, y) if (!(x <= y)) LOG(FATAL) << #x << " <= " << #y \
                                                 << "failed"
#define CHECK_NE(x, y) if (!(x != y)) LOG(FATAL) << #x << " != " << #y \
                                                 << "failed"
#define CHECK_EQ(x, y) if (!(x == y)) LOG(FATAL) << #x << " == " << #y \
                                                 << "failed"
#define DLOG(x) LOG(x)
#define VLOG(x) LOG(x)
#define DCHECK(x) CHECK(x)

#endif  // CHROMIUMOS_WIDE_PROFILING_MYBASE_BASE_LOGGING_H_
