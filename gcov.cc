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


#include "gflags/gflags.h"
#include "gcov.h"

// For different GCC versions, the gcov version number is:
//   4.6: 0x34303670
//   4.7: 0x3430372a
DEFINE_uint64(gcov_version, 0x3430372a,
              "Gcov version number.");

const uint32 GCOV_TAG_AFDO_FILE_NAMES = 0xaa000000;
const uint32 GCOV_TAG_AFDO_FUNCTION = 0xac000000;
const uint32 GCOV_TAG_MODULE_GROUPING = 0xae000000;
const uint32 GCOV_TAG_AFDO_WORKING_SET = 0xaf000000;
const uint32 GCOV_DATA_MAGIC = 0x67636461; /* "gcda" */
const char *GCOV_ELF_SECTION_NAME = ".gnu.switches.text";

#define GCOV_BLOCK_SIZE (1 << 10)

struct gcov_var {
  FILE *file;
  uint32 start;
  uint32 offset;
  uint32 length;
  uint32 overread;
  int32 error;
  int32 mode;
  uint64 alloc;
  uint32 *buffer;
};

static struct gcov_var gcov_var;

static void gcov_write_block(unsigned size) {
  if (fwrite(gcov_var.buffer, size << 2, 1, gcov_var.file) != 1) {
    gcov_var.error = 1;
  }
  gcov_var.start += size;
  gcov_var.offset -= size;
}

int gcov_open(const char *name, int mode) {
  CHECK(!gcov_var.file);
  gcov_var.start = 0;
  gcov_var.offset = gcov_var.length = 0;
  gcov_var.overread = -1u;
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
    if (gcov_var.offset && gcov_var.mode < 0) {
      gcov_write_block(gcov_var.offset);
    }
    fclose(gcov_var.file);
    gcov_var.file = 0;
    gcov_var.length = 0;
  }
  gcov_var.mode = 0;
  int err = gcov_var.error;
  return err;
}

static inline void gcov_allocate(unsigned length) {
  uint64 new_size = gcov_var.alloc;

  if (!new_size) {
    new_size = GCOV_BLOCK_SIZE;
  }
  new_size += length;
  new_size *= 2;

  gcov_var.alloc = new_size;
  gcov_var.buffer =
      static_cast<uint32 *>(realloc(gcov_var.buffer, new_size << 2));
}

static inline uint32 *gcov_write_words(unsigned words) {
  uint32 *result;

  CHECK_LT(gcov_var.mode, 0);
  if (gcov_var.offset + words > gcov_var.alloc) {
    gcov_allocate(gcov_var.offset + words);
  }
  result = &gcov_var.buffer[gcov_var.offset];
  gcov_var.offset += words;

  return result;
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
    alloc = (length + 4) >> 2;
  }

  buffer = gcov_write_words(1 + alloc);
  buffer[0] = alloc;
  buffer[alloc] = 0;
  memcpy(&buffer[1], string, length);
}

static inline const uint32 *gcov_read_words(unsigned words) {
  const uint32 *result;
  unsigned excess = gcov_var.length - gcov_var.offset;

  CHECK_GT(gcov_var.mode, 0);
  CHECK(words < GCOV_BLOCK_SIZE);
  if (excess < words) {
    gcov_var.start += gcov_var.offset;
    memmove(gcov_var.buffer, gcov_var.buffer + gcov_var.offset, excess * 4);
    gcov_var.offset = 0;
    gcov_var.length = excess;
    if (gcov_var.length + words > gcov_var.alloc) {
      gcov_allocate(gcov_var.length + words);
    }
    excess = gcov_var.alloc - gcov_var.length;
    excess = fread(gcov_var.buffer + gcov_var.length,
                   1, excess << 2, gcov_var.file) >> 2;
    gcov_var.length += excess;
    if (gcov_var.length < words) {
      gcov_var.overread += words - gcov_var.length;
      gcov_var.length = 0;
      return 0;
    }
  }
  result = &gcov_var.buffer[gcov_var.offset];
  gcov_var.offset += words;
  return result;
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

  return (const char *) gcov_read_words (length);
}
