// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/logging.h"

#include "chromiumos-wide-profiling/compat/string.h"
#include "chromiumos-wide-profiling/compat/test.h"
#include "chromiumos-wide-profiling/perf_parser.h"
#include "chromiumos-wide-profiling/perf_reader.h"
#include "chromiumos-wide-profiling/perf_test_files.h"
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
  testing::ExampleMmapEvent(
      1001, 0x1c1000, 0x1000, 0, "/usr/lib/foo.so",
      testing::SampleInfo().Tid(1001)).WriteTo(&input);        // 0
  // becomes: 0x0000, 0x1000, 0
  testing::ExampleMmapEvent(
      1001, 0x1c3000, 0x2000, 0x2000, "/usr/lib/bar.so",
      testing::SampleInfo().Tid(1001)).WriteTo(&input);        // 1
  // becomes: 0x1000, 0x2000, 0

  // PERF_RECORD_MMAP2
  testing::ExampleMmap2Event(
      1002, 0x2c1000, 0x2000, 0, "/usr/lib/baz.so",
      testing::SampleInfo().Tid(1002)).WriteTo(&input);        // 2
  // becomes: 0x0000, 0x2000, 0
  testing::ExampleMmap2Event(
      1002, 0x2c3000, 0x1000, 0x3000, "/usr/lib/xyz.so",
      testing::SampleInfo().Tid(1002)).WriteTo(&input);        // 3
  // becomes: 0x1000, 0x1000, 0

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001))  // 4
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c100a).Tid(1001))  // 5
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c3fff).Tid(1001))  // 6
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c2bad).Tid(1001))  // 7 (not mapped)
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c100a).Tid(1002))  // 8
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c5bad).Tid(1002))  // 9 (not mapped)
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c300b).Tid(1002))  // 10
      .WriteTo(&input);

  // not mapped yet:
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c400b).Tid(1002))  // 11
      .WriteTo(&input);
  testing::ExampleMmap2Event(
      1002, 0x2c4000, 0x1000, 0, "/usr/lib/new.so",
      testing::SampleInfo().Tid(1002)).WriteTo(&input);        // 12
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000002c400b).Tid(1002))  // 13
      .WriteTo(&input);

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

TEST(PerfParserTest, DsoInfoHasBuildId) {
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
  testing::ExampleMmapEvent(
      1001, 0x1c1000, 0x1000, 0, "/usr/lib/foo.so",
      testing::SampleInfo().Tid(1001)).WriteTo(&input);        // 0
  // becomes: 0x0000, 0x1000, 0
  testing::ExampleMmapEvent(
      1001, 0x1c3000, 0x2000, 0x2000, "/usr/lib/bar.so",
      testing::SampleInfo().Tid(1001)).WriteTo(&input);        // 1
  // becomes: 0x1000, 0x2000, 0

  // PERF_RECORD_HEADER_BUILDID                                // N/A
  string build_id_filename("/usr/lib/foo.so\0", 2*sizeof(u64));
  ASSERT_EQ(0, build_id_filename.size() % sizeof(u64)) << "Sanity check";
  const size_t event_size =
      sizeof(struct build_id_event) +
      build_id_filename.size();
  const struct build_id_event event = {
    .header = {
      .type = PERF_RECORD_HEADER_BUILD_ID,
      .misc = 0,
      .size = static_cast<u16>(event_size),
    },
    .pid = -1,
    .build_id = {0xde, 0xad, 0xf0, 0x0d },
  };
  input.write(reinterpret_cast<const char*>(&event), sizeof(event));
  input.write(build_id_filename.data(), build_id_filename.size());

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001))  // 2
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(
      testing::SampleInfo().Ip(0x00000000001c300a).Tid(1001))  // 3
      .WriteTo(&input);

  //
  // Parse input.
  //

  PerfParser::Options options;
  options.sample_mapping_percentage_threshold = 0;
  PerfParser parser(options);
  EXPECT_TRUE(parser.ReadFromString(input.str()));
  EXPECT_TRUE(parser.ParseRawEvents());
  EXPECT_EQ(2, parser.stats().num_mmap_events);
  EXPECT_EQ(2, parser.stats().num_sample_events);
  EXPECT_EQ(2, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent>& events = parser.parsed_events();
  ASSERT_EQ(4, events.size());

  EXPECT_EQ("/usr/lib/foo.so", events[2].dso_and_offset.dso_name());
  EXPECT_EQ("deadf00d00000000000000000000000000000000",
            events[2].dso_and_offset.build_id());
  EXPECT_EQ("/usr/lib/bar.so", events[3].dso_and_offset.dso_name());
  EXPECT_EQ("", events[3].dso_and_offset.build_id());
}

TEST(PerfParserTest, HandlesFinishedRoundEventsAndSortsByTime) {
  // For now at least, we are ignoring PERF_RECORD_FINISHED_ROUND events.

  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP |
                                              PERF_SAMPLE_TID |
                                              PERF_SAMPLE_TIME,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP
  testing::ExampleMmapEvent(
      1001, 0x1c1000, 0x1000, 0, "/usr/lib/foo.so",
      testing::SampleInfo().Tid(1001).Time(12300010)).WriteTo(&input);
  // becomes: 0x0000, 0x1000, 0


  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(                                // 1
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001).Time(12300020))
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(                                // 2
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001).Time(12300030))
      .WriteTo(&input);
  // PERF_RECORD_FINISHED_ROUND
  testing::FinishedRoundEvent().WriteTo(&input);                  // N/A

  // PERF_RECORD_SAMPLE
  testing::ExamplePerfSampleEvent(                                // 3
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001).Time(12300050))
      .WriteTo(&input);
  testing::ExamplePerfSampleEvent(                                // 4
      testing::SampleInfo().Ip(0x00000000001c1000).Tid(1001).Time(12300040))
      .WriteTo(&input);
  // PERF_RECORD_FINISHED_ROUND
  testing::FinishedRoundEvent().WriteTo(&input);                  // N/A

  //
  // Parse input.
  //

  PerfParser::Options options;
  options.sample_mapping_percentage_threshold = 0;
  options.do_remap = true;
  PerfParser parser(options);
  EXPECT_TRUE(parser.ReadFromString(input.str()));
  EXPECT_TRUE(parser.ParseRawEvents());
  EXPECT_EQ(1, parser.stats().num_mmap_events);
  EXPECT_EQ(4, parser.stats().num_sample_events);
  EXPECT_EQ(4, parser.stats().num_sample_events_mapped);

  const std::vector<ParsedEvent>& events = parser.parsed_events();
  ASSERT_EQ(5, events.size());

  struct perf_sample sample;

  EXPECT_EQ(PERF_RECORD_MMAP, events[0].raw_event->header.type);
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[1].raw_event->header.type);
  parser.ReadPerfSampleInfo(*events[1].raw_event, &sample);
  EXPECT_EQ(12300020, sample.time);
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[2].raw_event->header.type);
  parser.ReadPerfSampleInfo(*events[2].raw_event, &sample);
  EXPECT_EQ(12300030, sample.time);
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[3].raw_event->header.type);
  parser.ReadPerfSampleInfo(*events[3].raw_event, &sample);
  EXPECT_EQ(12300050, sample.time);
  EXPECT_EQ(PERF_RECORD_SAMPLE, events[4].raw_event->header.type);
  parser.ReadPerfSampleInfo(*events[4].raw_event, &sample);
  EXPECT_EQ(12300040, sample.time);

  const std::vector<ParsedEvent*>& sorted_events =
      parser.GetEventsSortedByTime();
  ASSERT_EQ(5, sorted_events.size());

  EXPECT_EQ(PERF_RECORD_MMAP, sorted_events[0]->raw_event->header.type);
  EXPECT_EQ(PERF_RECORD_SAMPLE, sorted_events[1]->raw_event->header.type);
  parser.ReadPerfSampleInfo(*sorted_events[1]->raw_event, &sample);
  EXPECT_EQ(12300020, sample.time);
  EXPECT_EQ(PERF_RECORD_SAMPLE, sorted_events[2]->raw_event->header.type);
  parser.ReadPerfSampleInfo(*sorted_events[2]->raw_event, &sample);
  EXPECT_EQ(12300030, sample.time);
  EXPECT_EQ(PERF_RECORD_SAMPLE, sorted_events[3]->raw_event->header.type);
  parser.ReadPerfSampleInfo(*sorted_events[3]->raw_event, &sample);
  EXPECT_EQ(12300040, sample.time);
  EXPECT_EQ(PERF_RECORD_SAMPLE, sorted_events[4]->raw_event->header.type);
  parser.ReadPerfSampleInfo(*sorted_events[4]->raw_event, &sample);
  EXPECT_EQ(12300050, sample.time);
}

}  // namespace quipper

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
