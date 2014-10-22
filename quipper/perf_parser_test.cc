// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/logging.h"

#include "perf_parser.h"
#include "perf_reader.h"
#include "perf_test_files.h"
#include "quipper_string.h"
#include "quipper_test.h"
#include "scoped_temp_path.h"
#include "test_utils.h"

namespace quipper {

namespace {

void CheckChronologicalOrderOfEvents(const PerfReader& reader,
                                     const std::vector<ParsedEvent*>& events) {
  // Here a valid PerfReader is needed to read the sample info because
  // ReadPerfSampleInfo() uses the |sample_type_| member of PerfReader to
  // determine which sample info fields are present.
  struct perf_sample sample_info;
  CHECK(reader.ReadPerfSampleInfo(*events[0]->raw_event, &sample_info));
  uint64_t prev_time = sample_info.time;
  for (unsigned int i = 1; i < events.size(); ++i) {
    struct perf_sample sample_info;
    CHECK(reader.ReadPerfSampleInfo(*events[i]->raw_event, &sample_info));
    CHECK_LE(prev_time, sample_info.time);
    prev_time = sample_info.time;
  }
}

void ReadFileAndCheckInternals(const string& input_perf_data,
                               PerfParser* parser) {
  PerfParser::Options options;
  options.do_remap = true;
  parser->set_options(options);
  ASSERT_TRUE(parser->ReadFile(input_perf_data));
  parser->ParseRawEvents();

  // Check perf event stats.
  const PerfEventStats& stats = parser->stats();
  EXPECT_GT(stats.num_sample_events, 0U);
  EXPECT_GT(stats.num_mmap_events, 0U);
  EXPECT_GT(stats.num_sample_events_mapped, 0U);
  EXPECT_TRUE(stats.did_remap);
}

}  // namespace

TEST(PerfParserTest, TestDSOAndOffsetConstructor) {
  // DSOAndOffset contains a pointer to a dso info struct. Make sure this is
  // initialized in a way such that DSOAndOffset::dso_name() executes without
  // segfault and returns an empty string.
  ParsedEvent::DSOAndOffset dso_and_offset;
  EXPECT_TRUE(dso_and_offset.dso_name().empty());
}

TEST(PerfParserTest, Test1Cycle) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfDataFiles);
       ++i) {
    const string test_file = perf_test_files::kPerfDataFiles[i];
    string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing " << input_perf_data;

    PerfParser parser(GetTestOptions());
    ASSERT_TRUE(parser.ReadFile(input_perf_data));
    parser.ParseRawEvents();

    CHECK_GT(parser.GetEventsSortedByTime().size(), 0U);
    CheckChronologicalOrderOfEvents(parser, parser.GetEventsSortedByTime());

    // Check perf event stats.
    const PerfEventStats& stats = parser.stats();
    EXPECT_GT(stats.num_sample_events, 0U);
    EXPECT_GT(stats.num_mmap_events, 0U);
    EXPECT_GT(stats.num_sample_events_mapped, 0U);
    EXPECT_FALSE(stats.did_remap);

    string output_perf_data = output_path + test_file + ".parse.out";
    ASSERT_TRUE(parser.WriteFile(output_perf_data));

    EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data));
    EXPECT_TRUE(ComparePerfBuildIDLists(input_perf_data, output_perf_data));
  }
}

TEST(PerfParserTest, TestNormalProcessing) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfDataFiles);
       ++i) {
    const string test_file = perf_test_files::kPerfDataFiles[i];
    string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing " << input_perf_data;

    PerfParser parser(GetTestOptions());
    ReadFileAndCheckInternals(input_perf_data, &parser);

    string output_perf_data = output_path + test_file + ".parse.remap.out";
    ASSERT_TRUE(parser.WriteFile(output_perf_data));

    // Remapped addresses should not match the original addresses.
    EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data));

    PerfParser remap_parser(GetTestOptions());
    ReadFileAndCheckInternals(output_perf_data, &remap_parser);
    string output_perf_data2 = output_path + test_file + ".parse.remap2.out";
    ASSERT_TRUE(remap_parser.WriteFile(output_perf_data2));

    // Remapping again should produce the same addresses.
    EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data2));
    EXPECT_TRUE(ComparePerfBuildIDLists(output_perf_data, output_perf_data2));
  }
}

TEST(PerfParserTest, TestPipedProcessing) {
  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfPipedDataFiles);
       ++i) {
    string input_perf_data =
        GetTestInputFilePath(perf_test_files::kPerfPipedDataFiles[i]);
    LOG(INFO) << "Testing " << input_perf_data;

    PerfParser parser(GetTestOptions());
    ReadFileAndCheckInternals(input_perf_data, &parser);
  }
}

}  // namespace quipper

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
