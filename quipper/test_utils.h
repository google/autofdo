// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TEST_UTILS_H_
#define TEST_UTILS_H_

#include <map>
#include <string>
#include <vector>

#include "base/basictypes.h"

#include "perf_parser.h"
#include "quipper_string.h"

namespace quipper {

extern const char* kSupportedMetadata[];

// Container for all the metadata from one perf report.  The key is the metadata
// type, as shown in |kSupportedMetadata|.  The value is a vector of all the
// occurrences of that type.  For some types, there is only one occurrence.
typedef std::map<string, std::vector<string> > MetadataSet;

// Path to the perf executable.
string GetPerfPath();

// Converts a perf data filename to the full path.
string GetTestInputFilePath(const string& filename);

// Returns the size of a file in bytes.
long int GetFileSize(const string& filename);

// Returns true if the contents of the two files are the same, false otherwise.
bool CompareFileContents(const string& filename1, const string& filename2);

// Used to create a temporary file or directory.
// TODO(cwp-team): Move it to a different file and add unit tests to this class.
class ScopedTempPath {
 public:
  ScopedTempPath() {}
  // The temporary path will be removed when the object is destroyed.
  virtual ~ScopedTempPath();
  const string path() const {
    return path_;
  }
 protected:
  string path_;
 private:
  DISALLOW_COPY_AND_ASSIGN(ScopedTempPath);
};

class ScopedTempFile : public ScopedTempPath {
 public:
  // Create a temporary file.  If successful, the path will be stored in
  // |path_|.  If not, |path_| will be an empty string.
  ScopedTempFile();
};

class ScopedTempDir : public ScopedTempPath {
 public:
  // Create a temporary directory.  If successful, the path will be stored in
  // |path_|.  If not, |path_| will be an empty string.
  ScopedTempDir();
};

// Given a perf data file, get the list of build ids and create a map from
// filenames to build ids.
bool GetPerfBuildIDMap(const string& filename,
                       std::map<string, string>* output);

bool CheckPerfDataAgainstBaseline(const string& filename);

// Returns true if the perf buildid-lists are the same.
bool ComparePerfBuildIDLists(const string& file1, const string& file2);

// Returns options suitable for correctness tests.
PerfParser::Options GetTestOptions();

}  // namespace quipper

#endif  // TEST_UTILS_H_
