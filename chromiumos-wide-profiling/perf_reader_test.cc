// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "base/logging.h"

#include "chromiumos-wide-profiling/perf_reader.h"
#include "chromiumos-wide-profiling/perf_test_files.h"
#include "chromiumos-wide-profiling/quipper_string.h"
#include "chromiumos-wide-profiling/quipper_test.h"
#include "chromiumos-wide-profiling/scoped_temp_path.h"
#include "chromiumos-wide-profiling/test_perf_data.h"
#include "chromiumos-wide-profiling/test_utils.h"
#include "chromiumos-wide-profiling/utils.h"

namespace quipper {

namespace {

// Any run of perf should have MMAPs with the following substrings.
const char* kExpectedFilenameSubstrings[] = {
  "perf",
  "kernel",
  "libc",
};

void CheckNoDuplicates(const std::vector<string>& list) {
  std::set<string> list_as_set(list.begin(), list.end());
  if (list.size() != list_as_set.size())
    ADD_FAILURE() << "Given list has at least one duplicate";
}

void CheckForElementWithSubstring(string substring_to_find,
                                  const std::vector<string>& list) {
  std::vector<string>::const_iterator iter;
  for (iter = list.begin(); iter != list.end(); ++iter)
    if (iter->find(substring_to_find) != string::npos)
      return;
  ADD_FAILURE() << substring_to_find
                << " is not present in any of the elements of the given list";
}

void CreateFilenameToBuildIDMap(
    const std::vector<string>& filenames, unsigned int seed,
    std::map<string, string>* filenames_to_build_ids) {
  srand(seed);
  // Only use every other filename, so that half the filenames are unused.
  for (size_t i = 0; i < filenames.size(); i += 2) {
    u8 build_id[kBuildIDArraySize];
    for (size_t j = 0; j < kBuildIDArraySize; ++j)
      build_id[j] = rand_r(&seed);

    (*filenames_to_build_ids)[filenames[i]] =
        HexToString(build_id, kBuildIDArraySize);
  }
}

void CheckFilenameAndBuildIDMethods(const string& input_perf_data,
                                    const string& output_perf_data_prefix,
                                    unsigned int seed,
                                    PerfReader* reader) {
  // Check filenames.
  std::vector<string> filenames;
  reader->GetFilenames(&filenames);

  ASSERT_FALSE(filenames.empty());
  CheckNoDuplicates(filenames);
  for (size_t i = 0; i < arraysize(kExpectedFilenameSubstrings); ++i)
    CheckForElementWithSubstring(kExpectedFilenameSubstrings[i], filenames);

  std::set<string> filename_set;
  reader->GetFilenamesAsSet(&filename_set);

  // Make sure all MMAP filenames are in the set.
  for (const auto& event : reader->events()) {
    if (event->header.type == PERF_RECORD_MMAP) {
      EXPECT_TRUE(filename_set.find(event->mmap.filename) != filename_set.end())
          << event->mmap.filename << " is not present in the filename set";
    }
  }

  std::map<string, string> expected_map;
  reader->GetFilenamesToBuildIDs(&expected_map);

  // Inject some made up build ids.
  std::map<string, string> filenames_to_build_ids;
  CreateFilenameToBuildIDMap(filenames, seed, &filenames_to_build_ids);
  ASSERT_TRUE(reader->InjectBuildIDs(filenames_to_build_ids));

  // Reader should now correctly populate the filenames to build ids map.
  std::map<string, string>::const_iterator it;
  for (it = filenames_to_build_ids.begin();
       it != filenames_to_build_ids.end();
       ++it) {
    expected_map[it->first] = it->second;
  }
  std::map<string, string> reader_map;
  reader->GetFilenamesToBuildIDs(&reader_map);
  EXPECT_EQ(expected_map, reader_map);

  string output_perf_data1 = output_perf_data_prefix + ".parse.inject.out";
  ASSERT_TRUE(reader->WriteFile(output_perf_data1));

  // Perf should find the same build ids.
  std::map<string, string> perf_build_id_map;
  ASSERT_TRUE(GetPerfBuildIDMap(output_perf_data1, &perf_build_id_map));
  EXPECT_EQ(expected_map, perf_build_id_map);

  std::map<string, string> build_id_localizer;
  // Only localize the first half of the files which have build ids.
  for (size_t j = 0; j < filenames.size() / 2; ++j) {
    string old_filename = filenames[j];
    if (expected_map.find(old_filename) == expected_map.end())
      continue;
    string build_id = expected_map[old_filename];

    string new_filename = old_filename + ".local";
    filenames[j] = new_filename;
    build_id_localizer[build_id] = new_filename;
    expected_map[new_filename] = build_id;
    expected_map.erase(old_filename);
  }
  reader->Localize(build_id_localizer);

  // Filenames should be the same.
  std::vector<string> new_filenames;
  reader->GetFilenames(&new_filenames);
  std::sort(filenames.begin(), filenames.end());
  EXPECT_EQ(filenames, new_filenames);

  // Build ids should be updated.
  reader_map.clear();
  reader->GetFilenamesToBuildIDs(&reader_map);
  EXPECT_EQ(expected_map, reader_map);

  string output_perf_data2 = output_perf_data_prefix + ".parse.localize.out";
  ASSERT_TRUE(reader->WriteFile(output_perf_data2));

  perf_build_id_map.clear();
  ASSERT_TRUE(GetPerfBuildIDMap(output_perf_data2, &perf_build_id_map));
  EXPECT_EQ(expected_map, perf_build_id_map);

  std::map<string, string> filename_localizer;
  // Only localize every third filename.
  for (size_t j = 0; j < filenames.size(); j += 3) {
    string old_filename = filenames[j];
    string new_filename = old_filename + ".local2";
    filenames[j] = new_filename;
    filename_localizer[old_filename] = new_filename;

    if (expected_map.find(old_filename) != expected_map.end()) {
      string build_id = expected_map[old_filename];
      expected_map[new_filename] = build_id;
      expected_map.erase(old_filename);
    }
  }
  reader->LocalizeUsingFilenames(filename_localizer);

  // Filenames should be the same.
  new_filenames.clear();
  reader->GetFilenames(&new_filenames);
  std::sort(filenames.begin(), filenames.end());
  EXPECT_EQ(filenames, new_filenames);

  // Build ids should be updated.
  reader_map.clear();
  reader->GetFilenamesToBuildIDs(&reader_map);
  EXPECT_EQ(expected_map, reader_map);

  string output_perf_data3 = output_perf_data_prefix + ".parse.localize2.out";
  ASSERT_TRUE(reader->WriteFile(output_perf_data3));

  perf_build_id_map.clear();
  ASSERT_TRUE(GetPerfBuildIDMap(output_perf_data3, &perf_build_id_map));
  EXPECT_EQ(expected_map, perf_build_id_map);
}

}  // namespace

TEST(PerfReaderTest, NormalModePerfData) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  int seed = 0;
  for (const char* test_file : perf_test_files::kPerfDataFiles) {
    string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing " << input_perf_data;
    string output_perf_data = output_path + test_file + ".pr.out";
    PerfReader pr;
    ASSERT_TRUE(pr.ReadFile(input_perf_data));
    ASSERT_TRUE(pr.WriteFile(output_perf_data));

    EXPECT_TRUE(CheckPerfDataAgainstBaseline(input_perf_data));
    EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data));
    EXPECT_TRUE(ComparePerfBuildIDLists(input_perf_data, output_perf_data));
    CheckFilenameAndBuildIDMethods(input_perf_data, output_path + test_file,
                                   seed, &pr);
    ++seed;
  }
}

TEST(PerfReaderTest, PipedModePerfData) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  int seed = 0;
  for (const char* test_file : perf_test_files::kPerfPipedDataFiles) {
    string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing " << input_perf_data;
    string output_perf_data = output_path + test_file + ".pr.out";
    PerfReader pr;
    ASSERT_TRUE(pr.ReadFile(input_perf_data));
    ASSERT_TRUE(pr.WriteFile(output_perf_data));

    EXPECT_TRUE(CheckPerfDataAgainstBaseline(input_perf_data));
    EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data));
    CheckFilenameAndBuildIDMethods(input_perf_data, output_path + test_file,
                                   seed, &pr);
    ++seed;
  }
}

TEST(PerfReaderTest, CorruptedFiles) {
  for (const char* test_file : perf_test_files::kCorruptedPerfPipedDataFiles) {
    string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing " << input_perf_data;
    ASSERT_TRUE(FileExists(input_perf_data)) << "Test file does not exist!";
    PerfReader pr;
    ASSERT_FALSE(pr.ReadFile(input_perf_data));
  }
}

TEST(PerfReaderTest, PerfizeBuildID) {
  string test = "f";
  PerfReader::PerfizeBuildIDString(&test);
  EXPECT_EQ("f000000000000000000000000000000000000000", test);
  PerfReader::PerfizeBuildIDString(&test);
  EXPECT_EQ("f000000000000000000000000000000000000000", test);

  test = "01234567890123456789012345678901234567890";
  PerfReader::PerfizeBuildIDString(&test);
  EXPECT_EQ("0123456789012345678901234567890123456789", test);
  PerfReader::PerfizeBuildIDString(&test);
  EXPECT_EQ("0123456789012345678901234567890123456789", test);
}

TEST(PerfReaderTest, UnperfizeBuildID) {
  string test = "f000000000000000000000000000000000000000";
  PerfReader::UnperfizeBuildIDString(&test);
  EXPECT_EQ("f0000000", test);
  PerfReader::UnperfizeBuildIDString(&test);
  EXPECT_EQ("f0000000", test);

  test = "0123456789012345678901234567890123456789";
  PerfReader::UnperfizeBuildIDString(&test);
  EXPECT_EQ("0123456789012345678901234567890123456789", test);

  test = "0000000000000000000000000000000000000000";
  PerfReader::UnperfizeBuildIDString(&test);
  EXPECT_EQ("00000000", test);
  PerfReader::UnperfizeBuildIDString(&test);
  EXPECT_EQ("00000000", test);

  test = "0000000000000000000000000000001000000000";
  PerfReader::UnperfizeBuildIDString(&test);
  EXPECT_EQ("00000000000000000000000000000010", test);
  PerfReader::UnperfizeBuildIDString(&test);
  EXPECT_EQ("00000000000000000000000000000010", test);
}

TEST(PerfReaderTest, ReadsAndWritesTraceMetadata) {
  std::stringstream input;

  const size_t attr_count = 1;

  // header
  testing::ExamplePerfDataFileHeader file_header(
      attr_count, 1 << HEADER_TRACING_DATA);
  file_header.WriteTo(&input);
  const perf_file_header &header = file_header.header();

  // attrs
  testing::ExamplePerfFileAttr_Tracepoint(73).WriteTo(&input);

  // data
  ASSERT_EQ(static_cast<u64>(input.tellp()), header.data.offset);
  testing::ExamplePerfSampleEvent_Tracepoint().WriteTo(&input);
  ASSERT_EQ(input.tellp(), file_header.data_end());

  // metadata

  const unsigned int metadata_count = 1;

  // HEADER_TRACING_DATA
  testing::ExampleTracingMetadata tracing_metadata(
      file_header.data_end() + metadata_count*sizeof(perf_file_section));

  // write metadata index entries
  tracing_metadata.index_entry().WriteTo(&input);
  // write metadata
  tracing_metadata.data().WriteTo(&input);

  //
  // Parse input.
  //

  PerfReader pr;
  EXPECT_TRUE(pr.ReadFromString(input.str()));
  EXPECT_EQ(tracing_metadata.data().value(), pr.tracing_data());

  // Write it out and read it in again, it should still be good:
  std::vector<char> output_perf_data;
  EXPECT_TRUE(pr.WriteToVector(&output_perf_data));
  EXPECT_TRUE(pr.ReadFromVector(output_perf_data));
  EXPECT_EQ(tracing_metadata.data().value(), pr.tracing_data());
}

TEST(PerfReaderTest, ReadsTracingMetadataEvent) {
  std::stringstream input;

  // pipe header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  const char x[] = "\x17\x08\x44tracing0.5BLAHBLAHBLAH....";
  const std::vector<char> trace_metadata(x, x+sizeof(x)-1);

  const tracing_data_event trace_event = {
    .header = {
      .type = PERF_RECORD_HEADER_TRACING_DATA,
      .misc = 0,
      .size = sizeof(tracing_data_event),
    },
    .size = static_cast<u32>(trace_metadata.size()),
  };

  input.write(reinterpret_cast<const char*>(&trace_event), sizeof(trace_event));
  input.write(trace_metadata.data(), trace_metadata.size());

  //
  // Parse input.
  //

  PerfReader pr;
  EXPECT_TRUE(pr.ReadFromString(input.str()));
  EXPECT_EQ(trace_metadata, pr.tracing_data());

  // Write it out and read it in again, tracing_data() should still be correct.
  // NB: It does not get written as an event, but in a metadata section.
  std::vector<char> output_perf_data;
  EXPECT_TRUE(pr.WriteToVector(&output_perf_data));
  EXPECT_TRUE(pr.ReadFromVector(output_perf_data));
  EXPECT_EQ(trace_metadata, pr.tracing_data());
}

// Regression test for http://crbug.com/427767
TEST(PerfReaderTest, CorrectlyReadsPerfEventAttrSize) {
  std::stringstream input;

  // pipe header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  struct old_perf_event_attr {
    __u32 type;
    __u32 size;
    __u64 config;
    // union {
            __u64 sample_period;
    //      __u64 sample_freq;
    // };
    __u64 sample_type;
    __u64 read_format;
    // Skip the rest of the fields from perf_event_attr to simulate an
    // older, smaller version of the struct.
  };

  struct old_attr_event {
    struct perf_event_header header;
    struct old_perf_event_attr attr;
    u64 id[];
  };


  const old_attr_event attr = {
    .header = {
      .type = PERF_RECORD_HEADER_ATTR,
      .misc = 0,
      // A count of 8 ids is carefully selected to make the event exceed
      // 96 bytes (sizeof(perf_event_attr)) so that the test fails instead of
      // crashes with the old code.
      .size = sizeof(old_attr_event) + 8*sizeof(u64),
    },
    .attr = {
      .type = 0,
      .size = sizeof(old_perf_event_attr),
      .config = 0,
      .sample_period = 10000001,
      .sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                     PERF_SAMPLE_ID | PERF_SAMPLE_CPU,
      .read_format = PERF_FORMAT_ID,
    },
  };

  input.write(reinterpret_cast<const char*>(&attr), sizeof(attr));
  for (u64 id : {301, 302, 303, 304, 305, 306, 307, 308})
    input.write(reinterpret_cast<const char*>(&id), sizeof(id));

  // Add some sample events so that there's something to over-read.
  const sample_event sample = {
    .header = {
      .type = PERF_RECORD_SAMPLE,
      .misc = 0,
      .size = sizeof(perf_event_header) + 5*sizeof(u64),
    }
  };

  for (int i = 0; i < 20; i++) {
    input.write(reinterpret_cast<const char*>(&sample), sizeof(sample));
    u64 qword = 0;
    for (int j = 0; j < 5; j++)
      input.write(reinterpret_cast<const char*>(&qword), sizeof(qword));
  }

  //
  // Parse input.
  //

  PerfReader pr;
  EXPECT_TRUE(pr.ReadFromString(input.str()));
  ASSERT_EQ(pr.attrs().size(), 1);
  const PerfFileAttr& actual_attr = pr.attrs()[0];
  ASSERT_EQ(8, actual_attr.ids.size());
  EXPECT_EQ(301, actual_attr.ids[0]);
  EXPECT_EQ(302, actual_attr.ids[1]);
  EXPECT_EQ(303, actual_attr.ids[2]);
  EXPECT_EQ(304, actual_attr.ids[3]);
}

TEST(PerfReaderTest, ReadsAndWritesSampleAndSampleIdAll) {
  using testing::PunU32U64;

  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  const u64 sample_type =      // * == in sample_id_all
      PERF_SAMPLE_IP |
      PERF_SAMPLE_TID |        // *
      PERF_SAMPLE_TIME |       // *
      PERF_SAMPLE_ADDR |
      PERF_SAMPLE_ID |         // *
      PERF_SAMPLE_STREAM_ID |  // *
      PERF_SAMPLE_CPU |        // *
      PERF_SAMPLE_PERIOD;
  const size_t num_sample_event_bits = 8;
  const size_t num_sample_id_bits = 5;
  // not tested:
  // PERF_SAMPLE_READ |
  // PERF_SAMPLE_RAW |
  // PERF_SAMPLE_CALLCHAIN |
  // PERF_SAMPLE_BRANCH_STACK |
  testing::ExamplePerfEventAttrEvent_Hardware(sample_type,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_SAMPLE
  const sample_event written_sample_event = {
    .header = {
      .type = PERF_RECORD_SAMPLE,
      .misc = PERF_RECORD_MISC_KERNEL,
      .size = sizeof(struct sample_event) + num_sample_event_bits*sizeof(u64),
    }
  };
  const u64 sample_event_array[] = {
    0xffffffff01234567,                  // IP
    PunU32U64{.v32={0x68d, 0x68e}}.v64,  // TID (u32 pid, tid)
    1415837014*1000000000ULL,            // TIME
    0x00007f999c38d15a,                  // ADDR
    2,                                   // ID
    1,                                   // STREAM_ID
    8,                                   // CPU
    10001,                               // PERIOD
  };
  ASSERT_EQ(written_sample_event.header.size,
            sizeof(written_sample_event.header) + sizeof(sample_event_array));
  input.write(reinterpret_cast<const char*>(&written_sample_event),
              sizeof(written_sample_event));
  input.write(reinterpret_cast<const char*>(sample_event_array),
              sizeof(sample_event_array));

  // PERF_RECORD_MMAP
  ASSERT_EQ(40, offsetof(struct mmap_event, filename));
  const size_t mmap_event_size =
      offsetof(struct mmap_event, filename) +
      10+6 /* ==16, nearest 64-bit boundary for filename */ +
      num_sample_id_bits*sizeof(u64);

  struct mmap_event written_mmap_event = {
    .header = {
      .type = PERF_RECORD_MMAP,
      .misc = 0,
      .size = mmap_event_size,
    },
    .pid = 0x68d, .tid = 0x68d,
    .start = 0x1d000,
    .len = 0x1000,
    .pgoff = 0,
    // .filename = ..., // written separately
  };
  const char mmap_filename[10+6] = "/dev/zero";
  const u64 mmap_sample_id[] = {
    PunU32U64{.v32={0x68d, 0x68e}}.v64,  // TID (u32 pid, tid)
    1415911367*1000000000ULL,            // TIME
    3,                                   // ID
    2,                                   // STREAM_ID
    9,                                   // CPU
  };
  const size_t pre_mmap_offset = input.tellp();
  input.write(reinterpret_cast<const char*>(&written_mmap_event),
              offsetof(struct mmap_event, filename));
  input.write(mmap_filename, 10+6);
  input.write(reinterpret_cast<const char*>(mmap_sample_id),
              sizeof(mmap_sample_id));
  const size_t written_mmap_size =
      static_cast<size_t>(input.tellp()) - pre_mmap_offset;
  ASSERT_EQ(written_mmap_event.header.size,
            static_cast<u64>(written_mmap_size));

  //
  // Parse input.
  //

  struct perf_sample sample;

  PerfReader pr1;
  EXPECT_TRUE(pr1.ReadFromString(input.str()));
  // Write it out and read it in again, the two should have the same data.
  std::vector<char> output_perf_data;
  EXPECT_TRUE(pr1.WriteToVector(&output_perf_data));
  PerfReader pr2;
  EXPECT_TRUE(pr2.ReadFromVector(output_perf_data));

  // Test both versions:
  for (PerfReader* pr : {&pr1, &pr2}) {
    // PERF_RECORD_HEADER_ATTR is added to attr(), not events().
    EXPECT_EQ(2, pr->events().size());

    const event_t* sample_event = pr->events()[0].get();
    EXPECT_EQ(PERF_RECORD_SAMPLE, sample_event->header.type);
    EXPECT_TRUE(pr->ReadPerfSampleInfo(*sample_event, &sample));
    EXPECT_EQ(0xffffffff01234567, sample.ip);
    EXPECT_EQ(0x68d, sample.pid);
    EXPECT_EQ(0x68e, sample.tid);
    EXPECT_EQ(1415837014*1000000000ULL, sample.time);
    EXPECT_EQ(0x00007f999c38d15a, sample.addr);
    EXPECT_EQ(2, sample.id);
    EXPECT_EQ(1, sample.stream_id);
    EXPECT_EQ(8, sample.cpu);
    EXPECT_EQ(10001, sample.period);

    const event_t* mmap_event = pr->events()[1].get();
    EXPECT_EQ(PERF_RECORD_MMAP, mmap_event->header.type);
    EXPECT_TRUE(pr->ReadPerfSampleInfo(*mmap_event, &sample));
    EXPECT_EQ(0x68d, sample.pid);
    EXPECT_EQ(0x68e, sample.tid);
    EXPECT_EQ(1415911367*1000000000ULL, sample.time);
    EXPECT_EQ(3, sample.id);
    EXPECT_EQ(2, sample.stream_id);
    EXPECT_EQ(9, sample.cpu);
  }
}

// Test that PERF_SAMPLE_IDENTIFIER is parsed correctly. This field
// is in a different place in PERF_RECORD_SAMPLE events compared to the
// struct sample_id placed at the end of all other events.
TEST(PerfReaderTest, ReadsAndWritesPerfSampleIdentifier) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IDENTIFIER |
                                              PERF_SAMPLE_IP |
                                              PERF_SAMPLE_TID,
                                              true /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_SAMPLE
  const sample_event written_sample_event = {
    .header = {
      .type = PERF_RECORD_SAMPLE,
      .misc = PERF_RECORD_MISC_KERNEL,
      .size = sizeof(struct sample_event) + 3*sizeof(u64),
    }
  };
  const u64 sample_event_array[] = {
    0x00000000deadbeef,  // IDENTIFIER
    0x00007f999c38d15a,  // IP
    0x0000068d0000068d,  // TID (u32 pid, tid)
  };
  ASSERT_EQ(written_sample_event.header.size,
            sizeof(written_sample_event.header) + sizeof(sample_event_array));
  input.write(reinterpret_cast<const char*>(&written_sample_event),
              sizeof(written_sample_event));
  input.write(reinterpret_cast<const char*>(sample_event_array),
              sizeof(sample_event_array));

  // PERF_RECORD_MMAP
  ASSERT_EQ(40, offsetof(struct mmap_event, filename));
  const size_t mmap_event_size =
      offsetof(struct mmap_event, filename) +
      10+6 /* ==16, nearest 64-bit boundary for filename */ +
      2*sizeof(u64);

  struct mmap_event written_mmap_event = {
    .header = {
      .type = PERF_RECORD_MMAP,
      .misc = 0,
      .size = mmap_event_size,
    },
    .pid = 0x68d, .tid = 0x68d,
    .start = 0x1d000,
    .len = 0x1000,
    .pgoff = 0,
    // .filename = ..., // written separately
  };
  const char mmap_filename[10+6] = "/dev/zero";
  const u64 mmap_sample_id[] = {
    // NB: PERF_SAMPLE_IP is not part of sample_id
    0x0000068d0000068d,  // TID (u32 pid, tid)
    0x00000000f00dbaad,  // IDENTIFIER
  };
  const size_t pre_mmap_offset = input.tellp();
  input.write(reinterpret_cast<const char*>(&written_mmap_event),
              offsetof(struct mmap_event, filename));
  input.write(mmap_filename, 10+6);
  input.write(reinterpret_cast<const char*>(mmap_sample_id),
              sizeof(mmap_sample_id));
  const size_t written_mmap_size =
      static_cast<size_t>(input.tellp()) - pre_mmap_offset;
  ASSERT_EQ(written_mmap_event.header.size,
            static_cast<u64>(written_mmap_size));

  //
  // Parse input.
  //

  struct perf_sample sample;

  PerfReader pr1;
  EXPECT_TRUE(pr1.ReadFromString(input.str()));
  // Write it out and read it in again, the two should have the same data.
  std::vector<char> output_perf_data;
  EXPECT_TRUE(pr1.WriteToVector(&output_perf_data));
  PerfReader pr2;
  EXPECT_TRUE(pr2.ReadFromVector(output_perf_data));

  // Test both versions:
  for (PerfReader* pr : {&pr1, &pr2}) {
    // PERF_RECORD_HEADER_ATTR is added to attr(), not events().
    EXPECT_EQ(2, pr->events().size());

    const event_t* sample_event = pr->events()[0].get();
    EXPECT_EQ(PERF_RECORD_SAMPLE, sample_event->header.type);
    EXPECT_TRUE(pr->ReadPerfSampleInfo(*sample_event, &sample));
    EXPECT_EQ(0xdeadbeefULL, sample.id);

    const event_t* mmap_event = pr->events()[1].get();
    EXPECT_EQ(PERF_RECORD_MMAP, mmap_event->header.type);
    EXPECT_TRUE(pr->ReadPerfSampleInfo(*mmap_event, &sample));
    EXPECT_EQ(0xf00dbaadULL, sample.id);
  }
}

TEST(PerfReaderTest, ReadsAndWritesMmap2Events) {
  std::stringstream input;

  // header
  testing::ExamplePipedPerfDataFileHeader().WriteTo(&input);

  // data

  // PERF_RECORD_HEADER_ATTR
  testing::ExamplePerfEventAttrEvent_Hardware(PERF_SAMPLE_IP,
                                              false /*sample_id_all*/)
      .WriteTo(&input);

  // PERF_RECORD_MMAP2
  ASSERT_EQ(72, offsetof(struct mmap2_event, filename));
  const size_t mmap_event_size =
      offsetof(struct mmap2_event, filename) +
      10+6; /* ==16, nearest 64-bit boundary for filename */

  struct mmap2_event written_mmap_event = {
    .header = {
      .type = PERF_RECORD_MMAP2,
      .misc = 0,
      .size = mmap_event_size,
    },
    .pid = 0x68d, .tid = 0x68d,
    .start = 0x1d000,
    .len = 0x1000,
    .pgoff = 0,
    .maj = 6,
    .min = 7,
    .ino = 8,
    .ino_generation = 9,
    .prot = 1|2,  // == PROT_READ | PROT_WRITE
    .flags = 2,   // == MAP_PRIVATE
    // .filename = ..., // written separately
  };
  const char mmap_filename[10+6] = "/dev/zero";
  const size_t pre_mmap_offset = input.tellp();
  input.write(reinterpret_cast<const char*>(&written_mmap_event),
              offsetof(struct mmap2_event, filename));
  input.write(mmap_filename, 10+6);
  const size_t written_mmap_size =
      static_cast<size_t>(input.tellp()) - pre_mmap_offset;
  ASSERT_EQ(written_mmap_event.header.size,
            static_cast<u64>(written_mmap_size));

  //
  // Parse input.
  //

  PerfReader pr1;
  EXPECT_TRUE(pr1.ReadFromString(input.str()));
  // Write it out and read it in again, the two should have the same data.
  std::vector<char> output_perf_data;
  EXPECT_TRUE(pr1.WriteToVector(&output_perf_data));
  PerfReader pr2;
  EXPECT_TRUE(pr2.ReadFromVector(output_perf_data));

  // Test both versions:
  for (PerfReader* pr : {&pr1, &pr2}) {
    // PERF_RECORD_HEADER_ATTR is added to attr(), not events().
    EXPECT_EQ(1, pr->events().size());

    const event_t* mmap2_event = pr->events()[0].get();
    EXPECT_EQ(PERF_RECORD_MMAP2, mmap2_event->header.type);
    EXPECT_EQ(0x68d, mmap2_event->mmap2.pid);
    EXPECT_EQ(0x68d, mmap2_event->mmap2.tid);
    EXPECT_EQ(0x1d000, mmap2_event->mmap2.start);
    EXPECT_EQ(0x1000, mmap2_event->mmap2.len);
    EXPECT_EQ(0, mmap2_event->mmap2.pgoff);
    EXPECT_EQ(6, mmap2_event->mmap2.maj);
    EXPECT_EQ(7, mmap2_event->mmap2.min);
    EXPECT_EQ(8, mmap2_event->mmap2.ino);
    EXPECT_EQ(9, mmap2_event->mmap2.ino_generation);
    EXPECT_EQ(1|2, mmap2_event->mmap2.prot);
    EXPECT_EQ(2, mmap2_event->mmap2.flags);
  }
}

}  // namespace quipper

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
