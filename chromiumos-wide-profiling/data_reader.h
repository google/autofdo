// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_DATA_READER_H_
#define CHROMIUMOS_WIDE_PROFILING_DATA_READER_H_

#include <stddef.h>

#include "chromiumos-wide-profiling/compat/string.h"

namespace quipper {

class DataReader {
 public:
  virtual ~DataReader() {}

  // Moves the data read pointer to |offset| bytes from the beginning of the
  // data.
  virtual void SeekSet(size_t offset) = 0;

  // Returns the position of the data read pointer, in bytes from the beginning
  // of the data.
  virtual size_t Tell() const = 0;

  virtual size_t size() const {
    return size_;
  }

  // Reads raw data into |dest|. Returns true if it managed to read |size|
  // bytes.
  virtual bool ReadData(const size_t size, void* dest) = 0;

  // Like ReadData(), but prints an error if it doesn't read all |size| bytes.
  virtual bool ReadDataValue(const size_t size, const string& value_name,
                             void* dest);

  // Read a string. Returns true if it managed to read |size| bytes (excluding
  // null terminator). The actual string may be shorter than the number of bytes
  // requested.
  virtual bool ReadString(const size_t size, string* str) = 0;

 protected:
  // Size of the data source.
  size_t size_;
};

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_DATA_READER_H_
