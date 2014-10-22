// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "scoped_temp_path.h"

#include <errno.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "base/logging.h"

// Temporary paths follow this format, with the X's replaced by letters or
// digits. This cannot be defined as a const variable because mkstemp() and
// mkdtemp() requires an initialized but variable string as an argument.
#define TEMP_PATH_STRING    "/tmp/quipper.XXXXXX"

namespace quipper {

ScopedTempFile::ScopedTempFile() {
  char filename[] = TEMP_PATH_STRING;
  int fd = mkstemp(filename);
  if (fd == -1)
    return;
  close(fd);
  path_ = filename;
}

ScopedTempDir::ScopedTempDir() {
  char dirname[] = TEMP_PATH_STRING;
  const char* name = mkdtemp(dirname);
  if (!name)
    return;
  path_ = string(name) + "/";
}

ScopedTempPath::~ScopedTempPath() {
  if (!path_.empty() && remove(path_.c_str()))
    LOG(ERROR) << "Error while removing " << path_ << ", errno: " << errno;
}

}  // namespace quipper
