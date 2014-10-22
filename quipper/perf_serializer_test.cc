// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <sys/time.h>

#include <sstream>
#include <string>

#include "base/logging.h"

#include "perf_protobuf_io.h"
#include "perf_reader.h"
#include "perf_serializer.h"
#include "perf_test_files.h"
#include "quipper_string.h"
#include "quipper_test.h"
#include "scoped_temp_path.h"
#include "test_utils.h"
#include "utils.h"

namespace {

// Returns a string representation of an unsigned integer |value|.
string UintToString(uint64_t value) {
  stringstream ss;
  ss << value;
  return ss.str();
}

}  // namespace

namespace quipper {

namespace {

// Gets the timestamp from an event field in PerfDataProto.
const uint64_t GetSampleTimestampFromEventProto(
    const PerfDataProto_PerfEvent& event) {
  // Get SampleInfo from the correct type-specific event field for the event.
  if (event.has_mmap_event()) {
    return event.mmap_event().sample_info().sample_time_ns();
  } else if (event.has_sample_event()) {
    return event.sample_event().sample_time_ns();
  } else if (event.has_comm_event()) {
    return event.comm_event().sample_info().sample_time_ns();
  } else if (event.has_fork_event()) {
    return event.fork_event().sample_info().sample_time_ns();
  } else if (event.has_lost_event()) {
    return event.lost_event().sample_info().sample_time_ns();
  } else if (event.has_throttle_event()) {
    return event.throttle_event().sample_info().sample_time_ns();
  } else if (event.has_read_event()) {
    return event.read_event().sample_info().sample_time_ns();
  }
  return 0;
}

// Verifies that |proto|'s events are in chronological order. No event should
// have an earlier timestamp than a preceding event.
void CheckChronologicalOrderOfSerializedEvents(const PerfDataProto& proto) {
  uint64_t prev_time_ns = 0;
  for (int i = 0; i < proto.events_size(); ++i) {
    // Compare each timestamp against the previous event's timestamp.
    uint64_t time_ns = GetSampleTimestampFromEventProto(proto.events(i));
    if (i > 0) {
      EXPECT_GE(time_ns, prev_time_ns);
    }
    prev_time_ns = time_ns;
  }
}

void SerializeAndDeserialize(const string& input,
                             const string& output,
                             bool do_remap,
                             bool discard_unused_events) {
  PerfDataProto perf_data_proto;
  PerfSerializer::Options options;
  options.do_remap = do_remap;
  options.discard_unused_events = discard_unused_events;
  options.sample_mapping_percentage_threshold = 100.0f;
  PerfSerializer serializer;
  serializer.set_options(options);
  EXPECT_TRUE(serializer.SerializeFromFile(input, &perf_data_proto));

  PerfSerializer deserializer;
  deserializer.set_options(options);
  EXPECT_TRUE(deserializer.DeserializeToFile(perf_data_proto, output));

  // Check perf event stats.
  const PerfEventStats& in_stats = serializer.stats();
  const PerfEventStats& out_stats = deserializer.stats();
  EXPECT_EQ(in_stats.num_sample_events, out_stats.num_sample_events);
  EXPECT_EQ(in_stats.num_mmap_events, out_stats.num_mmap_events);
  EXPECT_EQ(in_stats.num_fork_events, out_stats.num_fork_events);
  EXPECT_EQ(in_stats.num_exit_events, out_stats.num_exit_events);
  EXPECT_EQ(in_stats.num_sample_events_mapped,
            out_stats.num_sample_events_mapped);
  EXPECT_EQ(do_remap, in_stats.did_remap);
  EXPECT_EQ(do_remap, out_stats.did_remap);
}

void SerializeToFileAndBack(const string& input,
                            const string& output) {
  struct timeval pre_serialize_time;
  gettimeofday(&pre_serialize_time, NULL);

  // Serialize with and without sorting by chronological order.
  PerfDataProto input_perf_data_proto;

  // Serialize with and without sorting by chronological order.
  PerfSerializer sorted_serializer;
  sorted_serializer.set_serialize_sorted_events(true);
  EXPECT_TRUE(
      sorted_serializer.SerializeFromFile(input, &input_perf_data_proto));
  CheckChronologicalOrderOfSerializedEvents(input_perf_data_proto);

  input_perf_data_proto.Clear();
  PerfSerializer unsorted_serializer;
  unsorted_serializer.set_serialize_sorted_events(false);
  EXPECT_TRUE(
      unsorted_serializer.SerializeFromFile(input, &input_perf_data_proto));

  // Make sure the timestamp_sec was properly recorded.
  EXPECT_TRUE(input_perf_data_proto.has_timestamp_sec());
  // Check it against the current time.
  struct timeval post_serialize_time;
  gettimeofday(&post_serialize_time, NULL);
  EXPECT_GE(input_perf_data_proto.timestamp_sec(), pre_serialize_time.tv_sec);
  EXPECT_LE(input_perf_data_proto.timestamp_sec(), post_serialize_time.tv_sec);

  // Now store the protobuf into a file.
  ScopedTempFile input_file;
  EXPECT_FALSE(input_file.path().empty());
  string input_filename = input_file.path();
  ScopedTempFile output_file;
  EXPECT_FALSE(output_file.path().empty());
  string output_filename = output_file.path();

  EXPECT_TRUE(WriteProtobufToFile(input_perf_data_proto, input_filename));

  PerfDataProto output_perf_data_proto;
  EXPECT_TRUE(ReadProtobufFromFile(&output_perf_data_proto, input_filename));

  PerfSerializer deserializer;
  EXPECT_TRUE(deserializer.DeserializeToFile(output_perf_data_proto, output));

  EXPECT_TRUE(WriteProtobufToFile(output_perf_data_proto, output_filename));

  EXPECT_NE(GetFileSize(input_filename), 0);
  ASSERT_TRUE(CompareFileContents(input_filename, output_filename));

  remove(input_filename.c_str());
  remove(output_filename.c_str());
}

}  // namespace

TEST(PerfSerializerTest, Test1Cycle) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  // Read perf data using the PerfReader class.
  // Dump it to a protobuf.
  // Read the protobuf, and reconstruct the perf data.
  // TODO(sque): test exact number of events after discarding unused events.
  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfDataFiles);
       ++i) {
    PerfReader input_perf_reader, output_perf_reader, output_perf_reader1,
               output_perf_reader2;
    PerfDataProto perf_data_proto, perf_data_proto1;

    const char* test_file = perf_test_files::kPerfDataFiles[i];
    string input_perf_data = GetTestInputFilePath(test_file);
    string output_perf_data = output_path + test_file + ".serialized.out";
    string output_perf_data1 = output_path + test_file + ".serialized.1.out";

    LOG(INFO) << "Testing " << input_perf_data;
    input_perf_reader.ReadFile(input_perf_data);

    // Discard unused events for a pseudorandom selection of half the test data
    // files. The selection is based on the Md5sum prefix, so that the files can
    // be moved around in the |kPerfDataFiles| list.
    bool discard = (Md5Prefix(test_file) % 2 == 0);

    SerializeAndDeserialize(input_perf_data, output_perf_data, false, discard);
    output_perf_reader.ReadFile(output_perf_data);
    SerializeAndDeserialize(output_perf_data, output_perf_data1, false,
                            discard);
    output_perf_reader1.ReadFile(output_perf_data1);

    ASSERT_TRUE(CompareFileContents(output_perf_data, output_perf_data1));

    string output_perf_data2 = output_path + test_file + ".io.out";
    SerializeToFileAndBack(input_perf_data, output_perf_data2);
    output_perf_reader2.ReadFile(output_perf_data2);

    // Make sure the # of events do not increase.  They can decrease because
    // some unused non-sample events may be discarded.
    if (discard) {
      ASSERT_LE(output_perf_reader.events().size(),
                input_perf_reader.events().size());
    } else {
      ASSERT_EQ(output_perf_reader.events().size(),
                input_perf_reader.events().size());
    }
    ASSERT_EQ(output_perf_reader1.events().size(),
              output_perf_reader.events().size());
    ASSERT_EQ(output_perf_reader2.events().size(),
              input_perf_reader.events().size());

    EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data));
    EXPECT_TRUE(ComparePerfBuildIDLists(input_perf_data, output_perf_data));
    EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data2));
    EXPECT_TRUE(ComparePerfBuildIDLists(output_perf_data, output_perf_data2));
  }
}

TEST(PerfSerializerTest, TestRemap) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  // Read perf data using the PerfReader class with address remapping.
  // Dump it to a protobuf.
  // Read the protobuf, and reconstruct the perf data.
  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfDataFiles);
       ++i) {
    const string test_file = perf_test_files::kPerfDataFiles[i];
    const string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing " << input_perf_data;
    const string output_perf_data = output_path + test_file + ".ser.remap.out";
    SerializeAndDeserialize(input_perf_data, output_perf_data, true, true);
  }

  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfPipedDataFiles);
       ++i) {
    const string test_file = perf_test_files::kPerfPipedDataFiles[i];
    const string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing " << input_perf_data;
    const string output_perf_data = output_path + test_file + ".ser.remap.out";
    SerializeAndDeserialize(input_perf_data, output_perf_data, true, true);
  }
}

TEST(PerfSerializeTest, TestCommMd5s) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  // Replace command strings with their Md5sums.  Test size adjustment for
  // command strings.
  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfDataFiles);
       ++i) {
    const string test_file = perf_test_files::kPerfDataFiles[i];
    const string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing COMM Md5sum for " << input_perf_data;

    PerfDataProto perf_data_proto;
    PerfSerializer serializer;
    EXPECT_TRUE(serializer.SerializeFromFile(input_perf_data,
                                             &perf_data_proto));

    for (int j = 0; j < perf_data_proto.events_size(); ++j) {
      PerfDataProto_PerfEvent& event =
          *perf_data_proto.mutable_events(j);
      if (event.header().type() != PERF_RECORD_COMM)
        continue;
      CHECK(event.has_comm_event());

      string comm_md5_string =
          UintToString(event.mmap_event().filename_md5_prefix());
      // Make sure it fits in the comm string array, accounting for the null
      // terminator.
      struct comm_event dummy;
      if (comm_md5_string.size() > arraysize(dummy.comm) - 1)
        comm_md5_string.resize(arraysize(dummy.comm) - 1);
      event.mutable_comm_event()->set_comm(comm_md5_string);
    }

    PerfSerializer deserializer;
    const string output_perf_data = output_path + test_file + ".ser.comm.out";
    EXPECT_TRUE(deserializer.DeserializeToFile(perf_data_proto,
                                               output_perf_data));
    EXPECT_TRUE(CheckPerfDataAgainstBaseline(output_perf_data));
  }
}

TEST(PerfSerializeTest, TestMmapMd5s) {
  ScopedTempDir output_dir;
  ASSERT_FALSE(output_dir.path().empty());
  string output_path = output_dir.path();

  // Replace MMAP filename strings with their Md5sums.  Test size adjustment for
  // MMAP filename strings.
  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfDataFiles);
       ++i) {
    const string test_file = perf_test_files::kPerfDataFiles[i];
    const string input_perf_data = GetTestInputFilePath(test_file);
    LOG(INFO) << "Testing MMAP Md5sum for " << input_perf_data;

    PerfDataProto perf_data_proto;
    PerfSerializer serializer;
    EXPECT_TRUE(serializer.SerializeFromFile(input_perf_data,
                                             &perf_data_proto));

    for (int j = 0; j < perf_data_proto.events_size(); ++j) {
      PerfDataProto_PerfEvent& event =
          *perf_data_proto.mutable_events(j);
      if (event.header().type() != PERF_RECORD_MMAP)
        continue;
      CHECK(event.has_mmap_event());

      string filename_md5_string =
          UintToString(event.mmap_event().filename_md5_prefix());
      struct mmap_event dummy;
      // Make sure the Md5 prefix string can fit in the filename buffer,
      // including the null terminator
      if (filename_md5_string.size() > arraysize(dummy.filename) - 1)
        filename_md5_string.resize(arraysize(dummy.filename) - 1);
      event.mutable_mmap_event()->set_filename(filename_md5_string);
    }

    PerfSerializer deserializer;
    const string output_perf_data = output_path + test_file + ".ser.mmap.out";
    // Make sure the data can be deserialized after replacing the filenames with
    // Md5sum prefixes.  No need to check the output.
    EXPECT_TRUE(deserializer.DeserializeToFile(perf_data_proto,
                                               output_perf_data));
  }
}

TEST(PerfSerializerTest, TestProtoFiles) {
  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfDataProtoFiles);
       ++i) {
    string perf_data_proto_file =
        GetTestInputFilePath(perf_test_files::kPerfDataProtoFiles[i]);
    LOG(INFO) << "Testing " << perf_data_proto_file;
    std::vector<char> data;
    ASSERT_TRUE(FileToBuffer(perf_data_proto_file, &data));
    string text(data.begin(), data.end());

    PerfDataProto perf_data_proto;
    ASSERT_TRUE(TextFormat::ParseFromString(text, &perf_data_proto));
    LOG(INFO) << perf_data_proto.file_attrs_size();

    // Test deserializing.
    PerfSerializer perf_serializer;
    EXPECT_TRUE(perf_serializer.Deserialize(perf_data_proto));
  }
}

TEST(PerfSerializerTest, TestBuildIDs) {
  for (unsigned int i = 0;
       i < arraysize(perf_test_files::kPerfDataProtoFiles);
       ++i) {
    string perf_data_file =
        GetTestInputFilePath(perf_test_files::kPerfDataFiles[i]);
    LOG(INFO) << "Testing " << perf_data_file;

    // Serialize into a protobuf.
    PerfSerializer perf_serializer;
    PerfDataProto perf_data_proto;
    EXPECT_TRUE(perf_serializer.SerializeFromFile(perf_data_file,
                                                  &perf_data_proto));

    // Test a file with build ID filenames removed.
    for (int i = 0; i < perf_data_proto.build_ids_size(); ++i) {
      perf_data_proto.mutable_build_ids(i)->clear_filename();
    }
    PerfSerializer perf_serializer_build_ids;
    EXPECT_TRUE(perf_serializer_build_ids.Deserialize(perf_data_proto));
  }
}

}  // namespace quipper

int main(int argc, char * argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
