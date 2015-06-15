// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/data_writer.h"

#include "base/logging.h"

namespace quipper {

bool DataWriter::WriteDataValue(const void* src, const size_t size,
                                const string& value_name) {
  if (WriteData(src, size))
    return true;
  LOG(ERROR) << "Unable to write " << value_name << ". Requested " << size
             << " bytes, " << size_ - Tell() << " bytes remaining.";
  return false;
}

}  // namespace quipper
