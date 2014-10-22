// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_UTILS_H_
#define CHROMIUMOS_WIDE_PROFILING_UTILS_H_

#include <stdint.h>
#include <stdlib.h>  // for free()

#include <memory>
#include <string>
#include <vector>

#include "kernel/perf_internals.h"
#include "quipper_string.h"

namespace quipper {

struct FreeDeleter {
  inline void operator()(void* pointer) {
    free(pointer);
  }
};

template <typename T>
using malloced_unique_ptr = std::unique_ptr<T, FreeDeleter>;

// Given a valid open file handle |fp|, returns the size of the file.
int64_t GetFileSizeFromHandle(FILE* fp);

event_t* CallocMemoryForEvent(size_t size);
event_t* ReallocMemoryForEvent(event_t* event, size_t new_size);

build_id_event* CallocMemoryForBuildID(size_t size);

bool FileToBuffer(const string& filename, std::vector<char>* contents);

bool BufferToFile(const string& filename, const std::vector<char>& contents);

// Stores the value of |contents| within a compressed file with name |filename|.
bool BufferToGZFile(const string& filename, const std::vector<char>& contents);

// Reads a compressed file with name |filename| into |contents|.
bool GZFileToBuffer(const string& filename, std::vector<char>* contents);

uint64_t Md5Prefix(const string& input);

// Returns a string that represents |array| in hexadecimal.
string HexToString(const u8* array, size_t length);

// Converts |str| to a hexadecimal number, stored in |array|.  Returns true on
// success.  Only stores up to |length| bytes - if there are more characters in
// the string, they are ignored (but the function may still return true).
bool StringToHex(const string& str, u8* array, size_t length);

// Adjust |size| to blocks of |align_size|.  i.e. returns the smallest multiple
// of |align_size| that can fit |size|.
uint64_t AlignSize(uint64_t size, uint32_t align_size);

// Given a general perf sample format |sample_type|, return the fields of that
// format that are present in a sample for an event of type |event_type|.
//
// e.g. FORK and EXIT events have the fields {time, pid/tid, cpu, id}.
// Given a sample type with fields {ip, time, pid/tid, and period}, return
// the intersection of these two field sets: {time, pid/tid}.
//
// All field formats are bitfields, as defined by enum perf_event_sample_format
// in kernel/perf_event.h.
uint64_t GetSampleFieldsForEventType(uint32_t event_type, uint64_t sample_type);

// Returns the offset in bytes within a perf event structure at which the raw
// perf sample data is located.
uint64_t GetPerfSampleDataOffset(const event_t& event);

// Returns the size of the 8-byte-aligned memory for storing |string|.
size_t GetUint64AlignedStringLength(const string& str);

// Returns true iff the file exists.
bool FileExists(const string& filename);

// Reads the contents of a file into |data|.  Returns true on success, false if
// it fails.
bool ReadFileToData(const string& filename, std::vector<char>* data);

// Writes contents of |data| to a file with name |filename|, overwriting any
// existing file.  Returns true on success, false if it fails.
bool WriteDataToFile(const std::vector<char>& data, const string& filename);

// Executes |command| and stores stdout output in |output|.  Returns true on
// success, false otherwise.
bool RunCommandAndGetStdout(const string& command, std::vector<char>* output);

// Trim leading and trailing whitespace from |str|.
void TrimWhitespace(string* str);

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_UTILS_H_
