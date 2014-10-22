// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"

#include "conversion_utils.h"
#include "perf_test_files.h"
#include "quipper_test.h"
#include "scoped_temp_path.h"
#include "test_utils.h"
#include "utils.h"

namespace {

// The number for perf data files in our test files that we have protobuf text
// representation for.
const unsigned int kNumProtoTextFormatPerfFiles = 1;

}  // namespace

namespace quipper {

TEST(ConversionUtilsTest, TestTextOutput) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  // TODO(asharif): Generate more test files.
  for (unsigned int i = 0; i < kNumProtoTextFormatPerfFiles; ++i) {
    FormatAndFile input, output;
    string test_file = perf_test_files::kPerfDataFiles[i];

    input.filename = GetTestInputFilePath(test_file);
    input.format = kPerfFormat;
    output.filename = output_path + test_file + ".pb_text";
    output.format = kProtoTextFormat;
    EXPECT_TRUE(ConvertFile(input, output));

    string golden_file = GetTestInputFilePath(test_file + ".pb_text");
    LOG(INFO) << "golden: " << golden_file;
    LOG(INFO) << "output: " << output.filename;
    EXPECT_TRUE(CompareFileContents(golden_file, output.filename));
  }
}

}  // namespace quipper

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
