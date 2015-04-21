// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/scoped_temp_path.h"

#include <errno.h>
#include <ftw.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "base/logging.h"

// Temporary paths follow this format, with the X's replaced by letters or
// digits. This cannot be defined as a const variable because mkstemp() and
// mkdtemp() requires an initialized but variable string as an argument.
#define TEMP_PATH_STRING    "/tmp/quipper.XXXXXX"

namespace {

// Maximum number of directories that nftw() will hold open simultaneously.
const int kNumOpenFds = 4;

// Callback for nftw(). Deletes each file it is given.
int FileDeletionCallback(const char* path,
                         const struct stat* sb,
                         int /* type_flag */,
                         struct FTW* /* ftwbuf */) {
  if (path && remove(path))
    LOG(ERROR) << "Could not remove " << path << ", errno=" << errno;
  return 0;
}

}  // namespace

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
  // Recursively delete the path. Meaning of the flags:
  //   FTW_DEPTH: Handle directories after their contents.
  //   FTW_PHYS:  Do not follow symlinks.
  if (!path_.empty() &&
      nftw(path_.c_str(), FileDeletionCallback, kNumOpenFds,
           FTW_DEPTH | FTW_PHYS)) {
    LOG(ERROR) << "Error while using ftw() to remove " << path_;
  }
}

}  // namespace quipper
