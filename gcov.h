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

// The gcov api interface.

#ifndef AUTOFDO_GCOV_H_
#define AUTOFDO_GCOV_H_

#include "base/common.h"
#include "third_party/abseil/absl/flags/declare.h"

extern const uint32 GCOV_TAG_AFDO_SUMMARY;
extern const uint32 GCOV_TAG_AFDO_FILE_NAMES;
extern const uint32 GCOV_TAG_AFDO_FUNCTION;
extern const uint32 GCOV_TAG_MODULE_GROUPING;
extern const uint32 GCOV_TAG_AFDO_WORKING_SET;
extern const uint32 GCOV_DATA_MAGIC;
extern const char *GCOV_ELF_SECTION_NAME;

ABSL_DECLARE_FLAG(uint64_t, gcov_version);

enum hist_type {
  HIST_TYPE_INTERVAL,
  HIST_TYPE_POW2,
  HIST_TYPE_SINGLE_VALUE,
  HIST_TYPE_CONST_DELTA,
  HIST_TYPE_INDIR_CALL,
  HIST_TYPE_AVERAGE,
  HIST_TYPE_IOR,
  HIST_TYPE_INDIR_CALL_TOPN
};

int gcov_open(const char *name, int mode);

int gcov_close(void);

void gcov_write_unsigned(uint32 value);

void gcov_write_counter(uint64 value);

void gcov_write_string(const char *string);

uint32 gcov_read_unsigned(void);

uint64 gcov_read_counter(void);

const char * gcov_read_string(void);

#endif  // AUTOFDO_GCOV_H_
