// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/logging.h"

#include "chromiumos-wide-profiling/perf_parser.h"
#include "chromiumos-wide-profiling/perf_reader.h"
#include "chromiumos-wide-profiling/perf_test_files.h"
#include "chromiumos-wide-profiling/quipper_string.h"
#include "chromiumos-wide-profiling/quipper_test.h"
#include "chromiumos-wide-profiling/scoped_temp_path.h"
#include "chromiumos-wide-profiling/test_perf_data.h"
#include "chromiumos-wide-profiling/test_utils.h"

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

TEST(PerfParserTest, MapsSampleEventIp) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP |
                                              PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent_Tid(
      1001, 0x1c1000, 0x1000, 0, "/usr/lib/foo.so").WriteTo(&input);       // 0
  // becomes: 0x0000, 0x1000, 0
  testing::ExampleMmapEvent_Tid(
      1001, 0x1c3000, 0x2000, 0x2000, "/usr/lib/bar.so").WriteTo(&input);  // 1
  // becomes: 0x1000, 0x2000, 0

  // PERF_RECORD_MMAP2
  testing::ExampleMmap2Event_Tid(
      1002, 0x2c1000, 0x2000, 0, "/usr/lib/baz.so").WriteTo(&input);       // 2
  // becomes: 0x0000, 0x2000, 0
  testing::ExampleMmap2Event_Tid(
      1002, 0x2c3000, 0x1000, 0x3000, "/usr/lib/xyz.so").WriteTo(&input);  // 3
  // becomes: 0x1000, 0x1000, 0

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000001c1000, 1001, 1001).WriteTo(&input);  // 4
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000001c100a, 1001, 1001).WriteTo(&input);  // 5
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000001c3fff, 1001, 1001).WriteTo(&input);  // 6
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000001c2bad, 1001, 1001).WriteTo(&input);  // 7 (not mapped)
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000002c100a, 1002, 1002).WriteTo(&input);  // 8
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000002c5bad, 1002, 1002).WriteTo(&input);  // 9 (not mapped)
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000002c300b, 1002, 1002).WriteTo(&input);  // 10

  // not mapped yet:
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000002c400b, 1002, 1002).WriteTo(&input);  // 11
  testing::ExampleMmap2Event_Tid(
      1002, 0x2c4000, 0x1000, 0, "/usr/lib/new.so").WriteTo(&input);  // 12
  testing::ExamplePerfSampleEvent_IpTid(
      0x00000000002c400b, 1002, 1002).WriteTo(&input);  // 13

  //
  // Parse input.
  //

  PerfParser::Options options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(options);
  EXPECT_TRUE(parser.ReadFromString(input.str()));
  EXPECT_TRUE(parser.ParseRawEvents());
  EXPECT_EQ(5, parser.stats().num_mmap_events);
  EXPECT_EQ(9, parser.stats().num_sample_events);
  EXPECT_EQ(6, parser.stats().num_sample_events_mapped);


  const std::vector<ParsedEvent>& events = parser.parsed_events();
  ASSERT_EQ(14, events.size());

  // MMAPs

  EXPECT_EQ(PERF_RECORD_MMAP, events[0].raw_event->header.type);
  EXPECT_EQ("/usr/lib/foo.so", string(events[0].raw_event->mmap.filename));
  EXPECT_EQ(0x0000, events[0].raw_event->mmap.start);
  EXPECT_EQ(0x1000, events[0].raw_event->mmap.len);
  EXPECT_EQ(0, events[0].raw_event->mmap.pgoff);

  EXPECT_EQ(PERF_RECORD_MMAP, events[1].raw_event->header.type);
  EXPECT_EQ("/usr/lib/bar.so", string(events[1].raw_event->mmap.filename));
  EXPECT_EQ(0x1000, events[1].raw_event->mmap.start);
  EXPECT_EQ(0x2000, events[1].raw_event->mmap.len);
  EXPECT_EQ(0x2000, events[1].raw_event->mmap.pgoff);

  EXPECT_EQ(PERF_RECORD_MMAP2, events[2].raw_event->header.type);
  EXPECT_EQ("/usr/lib/baz.so", string(events[2].raw_event->mmap2.filename));
  EXPECT_EQ(0x0000, events[2].raw_event->mmap2.start);
  EXPECT_EQ(0x2000, events[2].raw_event->mmap2.len);
  EXPECT_EQ(0, events[2].raw_event->mmap2.pgoff);

  EXPECT_EQ(PERF_RECORD_MMAP2, events[3].raw_event->header.type);
  EXPECT_EQ("/usr/lib/xyz.so", string(events[3].raw_event->mmap2.filename));
  EXPECT_EQ(0x2000, events[3].raw_event->mmap2.start);
  EXPECT_EQ(0x1000, events[3].raw_event->mmap2.len);
  EXPECT_EQ(0x3000, events[3].raw_event->mmap2.pgoff);

  // SAMPLEs

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[4].raw_event->header.type);
  EXPECT_EQ("/usr/lib/foo.so", events[4].dso_and_offset.dso_name());
  EXPECT_EQ(0x0, events[4].dso_and_offset.offset());
  EXPECT_EQ(0x0, events[4].raw_event->sample.array[0]);

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[5].raw_event->header.type);
  EXPECT_EQ("/usr/lib/foo.so", events[5].dso_and_offset.dso_name());
  EXPECT_EQ(0xa, events[5].dso_and_offset.offset());
  EXPECT_EQ(0xa, events[5].raw_event->sample.array[0]);

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[6].raw_event->header.type);
  EXPECT_EQ("/usr/lib/bar.so", events[6].dso_and_offset.dso_name());
  EXPECT_EQ(0x2fff, events[6].dso_and_offset.offset());
  EXPECT_EQ(0x1fff, events[6].raw_event->sample.array[0]);

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[7].raw_event->header.type);
  EXPECT_EQ(0x00000000001c2bad, events[7].raw_event->sample.array[0]);

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[8].raw_event->header.type);
  EXPECT_EQ("/usr/lib/baz.so", events[8].dso_and_offset.dso_name());
  EXPECT_EQ(0xa, events[8].dso_and_offset.offset());
  EXPECT_EQ(0xa, events[8].raw_event->sample.array[0]);

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[9].raw_event->header.type);
  EXPECT_EQ(0x00000000002c5bad, events[9].raw_event->sample.array[0]);

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[10].raw_event->header.type);
  EXPECT_EQ("/usr/lib/xyz.so", events[10].dso_and_offset.dso_name());
  EXPECT_EQ(0x300b, events[10].dso_and_offset.offset());
  EXPECT_EQ(0x200b, events[10].raw_event->sample.array[0]);

  // not mapped yet:
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[11].raw_event->header.type);
  EXPECT_EQ(0x00000000002c400b, events[11].raw_event->sample.array[0]);

  EXPECT_EQ(PERF_RECORD_MMAP2, events[12].raw_event->header.type);
  EXPECT_EQ("/usr/lib/new.so", string(events[12].raw_event->mmap2.filename));
  EXPECT_EQ(0x3000, events[12].raw_event->mmap2.start);
  EXPECT_EQ(0x1000, events[12].raw_event->mmap2.len);
  EXPECT_EQ(0, events[12].raw_event->mmap2.pgoff);

  EXPECT_EQ(PERF_RECORD_SAMPLE, events[13].raw_event->header.type);
  EXPECT_EQ("/usr/lib/new.so", events[13].dso_and_offset.dso_name());
  EXPECT_EQ(0xb, events[13].dso_and_offset.offset());
  EXPECT_EQ(0x300b, events[13].raw_event->sample.array[0]);
}

}  // namespace quipper

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
