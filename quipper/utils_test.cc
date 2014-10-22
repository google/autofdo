// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "perf_protobuf_io.h"
#include "perf_reader.h"
#include "perf_serializer.h"
#include "quipper_test.h"
#include "test_utils.h"
#include "utils.h"

namespace quipper {

namespace {

const size_t kHexArraySize = 8;

}  // namespace

TEST(UtilsTest, TestMD5) {
  ASSERT_EQ(Md5Prefix(""), 0xd41d8cd98f00b204LL);
  ASSERT_EQ(Md5Prefix("The quick brown fox jumps over the lazy dog."),
            0xe4d909c290d0fb1cLL);
}

TEST(UtilsTest, TestAlignSize) {
  EXPECT_EQ(12, AlignSize(10, 4));
  EXPECT_EQ(12, AlignSize(12, 4));
  EXPECT_EQ(16, AlignSize(13, 4));
  EXPECT_EQ(100, AlignSize(97, 4));
  EXPECT_EQ(100, AlignSize(100, 4));
  EXPECT_EQ(104, AlignSize(100, 8));
  EXPECT_EQ(112, AlignSize(108, 8));
  EXPECT_EQ(112, AlignSize(112, 8));
}

TEST(UtilsTest, TestHexToString) {
  u8 hex_number[kHexArraySize];
  // Generate a sequence of bytes and check its hex string representation.
  for (size_t i = 0; i < arraysize(hex_number); ++i)
    hex_number[i] = i << i;
  EXPECT_EQ("0002081840a08080", HexToString(hex_number, arraysize(hex_number)));

  // Change the first and last bytes and check the new hex string.
  hex_number[0] = 0x8f;
  hex_number[arraysize(hex_number) - 1] = 0x64;
  EXPECT_EQ("8f02081840a08064", HexToString(hex_number, arraysize(hex_number)));
}

TEST(UtilsTest, TestGZRead) {
  std::vector<char> contents;
  GZFileToBuffer(GetTestInputFilePath("hello_world.txt.gz"), &contents);
  string string_contents(contents.begin(), contents.end());
  EXPECT_EQ(string_contents, "hello world\n");
}

TEST(UtilsTest, TestStringToHex) {
  u8 output[kHexArraySize], expected[kHexArraySize];

  // Use the same tests as in TestHexToString, except reversed.
  for (size_t i = 0; i < arraysize(expected); ++i)
    expected[i] = i << i;
  EXPECT_TRUE(StringToHex("0002081840a08080", output, arraysize(output)));
  for (size_t i = 0; i < arraysize(expected); ++i)
    EXPECT_EQ(expected[i], output[i]);

  expected[0] = 0x8f;
  expected[arraysize(expected) - 1] = 0x64;
  EXPECT_TRUE(StringToHex("8f02081840a080640123456789abcdef",
                          output, arraysize(output)));
  for (size_t i = 0; i < arraysize(expected); ++i)
    EXPECT_EQ(expected[i], output[i]);
}

}  // namespace quipper

int main(int argc, char * argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
