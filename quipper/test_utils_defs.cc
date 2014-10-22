// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strings/join.h"
#include "testing/base/public/googletest.h"
#include "third_party/quipper/utils.h"

namespace quipper {

string GetTestInputFilePath(const string& filename) {
  return StrCat(FLAGS_test_srcdir,
                "/google3/third_party/quipper/testdata/",
                filename);
}

string GetPerfPath() {
  return GetTestInputFilePath("perf");
}

}  // namespace quipper
