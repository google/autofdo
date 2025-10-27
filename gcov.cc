// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Define the flag used by gcov.

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


#include "gcov.h"
#include "third_party/abseil/absl/flags/flag.h"

// For different GCC versions, the gcov version number is:
//   4.6: 0x34303670
//   4.7: 0x3430372a
ABSL_FLAG(uint64_t, gcov_version, 0x3430372a,
              "Gcov version number.");

const uint32 GCOV_TAG_AFDO_SUMMARY = 0xa8000000;
const uint32 GCOV_TAG_AFDO_FILE_NAMES = 0xaa000000;
const uint32 GCOV_TAG_AFDO_FUNCTION = 0xac000000;
const uint32 GCOV_TAG_MODULE_GROUPING = 0xae000000;
const uint32 GCOV_TAG_AFDO_WORKING_SET = 0xaf000000;
const uint32 GCOV_DATA_MAGIC = 0x67636461; /* "gcda" */
const char *GCOV_ELF_SECTION_NAME = ".gnu.switches.text";

#define GCOV_BLOCK_BYTE_SIZE (1 << 14)

struct gcov_var {
  FILE *file;
  uint32 byte_offset;
  uint32 byte_length;
  int32 error;
  int32 mode;
  uint64 byte_alloc;
  char *buffer;
};

static struct gcov_var gcov_var;

static void gcov_write_block(unsigned byte_size) {
  if (fwrite(gcov_var.buffer, byte_size, 1, gcov_var.file) != 1) {
    gcov_var.error = 1;
  }
  gcov_var.byte_offset -= byte_size;
}

int gcov_open(const char *name, int mode) {
  CHECK(!gcov_var.file);
  gcov_var.byte_offset = gcov_var.byte_length = 0;
  gcov_var.error = 0;
  if (mode >= 0)
    gcov_var.file = fopen(name, (mode > 0) ? "rb" : "r+b");

  if (gcov_var.file) {
    gcov_var.mode = 1;
  } else if (mode <= 0) {
    gcov_var.file = fopen(name, "w+b");
    if (gcov_var.file) {
      gcov_var.mode = mode * 2 + 1;
    }
  }
  if (!gcov_var.file) {
    return 0;
  }

  setbuf(gcov_var.file, NULL);

  return 1;
}

int gcov_close(void) {
  if (gcov_var.file) {
    if (gcov_var.byte_offset && gcov_var.mode < 0) {
      gcov_write_block(gcov_var.byte_offset);
    }
    fclose(gcov_var.file);
    gcov_var.file = 0;
    gcov_var.byte_length = 0;
  }
  gcov_var.mode = 0;
  int err = gcov_var.error;
  return err;
}

static inline void gcov_allocate(unsigned byte_length) {
  uint64 new_byte_size = gcov_var.byte_alloc;

  if (!new_byte_size) {
    new_byte_size = GCOV_BLOCK_BYTE_SIZE;
  }
  new_byte_size += byte_length;
  new_byte_size *= 2;

  gcov_var.byte_alloc = new_byte_size;
  gcov_var.buffer =
      static_cast<char *>(realloc(gcov_var.buffer, new_byte_size));
}

static inline char *gcov_write_bytes(unsigned bytes) {
  char *result;

  CHECK_LT(gcov_var.mode, 0);
  if (gcov_var.byte_offset + bytes > gcov_var.byte_alloc) {
    gcov_allocate(gcov_var.byte_offset + bytes);
  }
  result = &gcov_var.buffer[gcov_var.byte_offset];
  gcov_var.byte_offset += bytes;

  return result;
}

static inline uint32 *gcov_write_words(unsigned words) {
  uint32 *result;
  unsigned bytes = words << 2;

  return reinterpret_cast<uint32 *>(gcov_write_bytes(bytes));
}


void gcov_write_unsigned(uint32 value) {
  uint32 *buffer = gcov_write_words(1);
  buffer[0] = value;
}

void gcov_write_counter(uint64 value) {
  uint32 *buffer = gcov_write_words(2);

  buffer[0] = static_cast<uint32>(value);
  buffer[1] = static_cast<uint32>(value >> 32);
}

void gcov_write_string(const char *string) {
  unsigned length = 0;
  unsigned alloc = 0;
  uint32 *buffer;

  if (string) {
    length = strlen(string);
    if (absl::GetFlag(FLAGS_gcov_version) >= 2) {
      // Length includes the terminating 0 and is saved in bytes.
      alloc = length + 1;
      char *byte_buffer = gcov_write_bytes(4 + alloc);
      byte_buffer[3 + alloc] = 0;
      buffer = reinterpret_cast<uint32 *>(byte_buffer);
    }
    else {
      // Length is saved in words and padding is added.
      alloc = (length + 4) >> 2;
      buffer = gcov_write_words(1 + alloc);
      buffer[alloc] = 0;
    }
  }

  buffer[0] = alloc;
  memcpy(&buffer[1], string, length);
}

static inline const char *gcov_read_bytes(unsigned bytes) {
  const char *result;
  unsigned excess_bytes = gcov_var.byte_length - gcov_var.byte_offset;

  CHECK_GT(gcov_var.mode, 0);
  CHECK(bytes < GCOV_BLOCK_BYTE_SIZE);
  if (excess_bytes < bytes) {
    memmove(gcov_var.buffer, gcov_var.buffer + gcov_var.byte_offset, excess_bytes);
    gcov_var.byte_offset = 0;
    gcov_var.byte_length = excess_bytes;
    if (gcov_var.byte_length + bytes > gcov_var.byte_alloc) {
      gcov_allocate(gcov_var.byte_length + bytes);
    }
    excess_bytes = gcov_var.byte_alloc - gcov_var.byte_length;
    excess_bytes = fread(gcov_var.buffer + gcov_var.byte_length,
                   1, excess_bytes, gcov_var.file);
    gcov_var.byte_length += excess_bytes;
    if (gcov_var.byte_length < bytes) {
      gcov_var.byte_length = 0;
      return 0;
    }
  }
  result = &gcov_var.buffer[gcov_var.byte_offset];
  gcov_var.byte_offset += bytes;
  return result;
}

static inline const uint32 *gcov_read_words(unsigned words) {
  unsigned bytes = words << 2;
  return reinterpret_cast<const uint32 *>(gcov_read_bytes(bytes));
}

uint32 gcov_read_unsigned(void) {
  uint32 value;
  const uint32 *buffer = gcov_read_words(1);

  if (!buffer) {
    return 0;
  }
  value = buffer[0];
  return value;
}

uint64 gcov_read_counter(void) {
  uint64 value;
  const uint32 *buffer = gcov_read_words(2);

  if (!buffer)
    return 0;
  value = buffer[0];
  value |= (static_cast<uint64>(buffer[1])) << 32;
  return value;
}

const char * gcov_read_string(void) {
  unsigned length = gcov_read_unsigned();

  if (!length) {
    return 0;
  }

  if (absl::GetFlag(FLAGS_gcov_version) >= 2) {
    return gcov_read_bytes (length);
  }
  else {
    return (const char *) gcov_read_words (length);
  }
}
