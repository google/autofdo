// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_PERF_READER_H_
#define CHROMIUMOS_WIDE_PROFILING_PERF_READER_H_

#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/macros.h"

#include "chromiumos-wide-profiling/compat/string.h"
#include "chromiumos-wide-profiling/kernel/perf_internals.h"
#include "chromiumos-wide-profiling/utils.h"

namespace quipper {

// This is becoming more like a partial struct perf_evsel
struct PerfFileAttr {
  struct perf_event_attr attr;
  string name;
  std::vector<u64> ids;
};

// Based on code in tools/perf/util/header.c, the metadata are of the following
// formats:

// Based on kernel/perf_internals.h
const size_t kBuildIDArraySize = 20;
const size_t kBuildIDStringLength = kBuildIDArraySize * 2;

struct CStringWithLength {
  u32 len;
  string str;
};

struct PerfStringMetadata {
  u32 type;
  std::vector<CStringWithLength> data;
};

struct PerfUint32Metadata {
  u32 type;
  std::vector<uint32_t> data;
};

struct PerfUint64Metadata {
  u32 type;
  std::vector<uint64_t> data;
};

typedef u32 num_siblings_type;

struct PerfCPUTopologyMetadata {
  std::vector<CStringWithLength> core_siblings;
  std::vector<CStringWithLength> thread_siblings;
};

struct PerfNodeTopologyMetadata {
  u32 id;
  u64 total_memory;
  u64 free_memory;
  CStringWithLength cpu_list;
};

class DataReader;
class DataWriter;

class PerfReader {
 public:
  PerfReader() : sample_type_(0),
                 read_format_(0),
                 is_cross_endian_(0) {}
  ~PerfReader();

  // Makes |build_id| fit the perf format, by either truncating it or adding
  // zeros to the end so that it has length kBuildIDStringLength.
  static void PerfizeBuildIDString(string* build_id);

  // Changes |build_id| to the best guess of what the build id was before going
  // through perf.  Specifically, it keeps removing trailing sequences of four
  // zero bytes (or eight '0' characters) until there are no more such
  // sequences, or the build id would be empty if the process were repeated.
  static void UnperfizeBuildIDString(string* build_id);

  bool ReadFile(const string& filename);
  bool ReadFromVector(const std::vector<char>& data);
  bool ReadFromString(const string& str);
  bool ReadFromPointer(const char* data, size_t size);
  bool ReadFromData(DataReader* data);

  // TODO(rohinmshah): GetSize should not use RegenerateHeader (so that it can
  // be const).  Ideally, RegenerateHeader would be deleted and instead of
  // having out_header_ as an instance variable, it would be computed
  // dynamically whenever needed.

  // Returns the size in bytes that would be written by any of the methods that
  // write the entire perf data file (WriteFile, WriteToPointer, etc).
  size_t GetSize();

  bool WriteFile(const string& filename);
  bool WriteToVector(std::vector<char>* data);
  bool WriteToString(string* str);
  bool WriteToPointer(char* buffer, size_t size);

  bool RegenerateHeader();

  // Stores the mapping from filenames to build ids in build_id_events_.
  // Returns true on success.
  // Note: If |filenames_to_build_ids| contains a mapping for a filename for
  // which there is already a build_id_event in build_id_events_, a duplicate
  // build_id_event will be created, and the old build_id_event will NOT be
  // deleted.
  bool InjectBuildIDs(const std::map<string, string>& filenames_to_build_ids);

  // Replaces existing filenames with filenames from |build_ids_to_filenames|
  // by joining on build ids.  If a build id in |build_ids_to_filenames| is not
  // present in this parser, it is ignored.
  bool Localize(const std::map<string, string>& build_ids_to_filenames);

  // Same as Localize, but joins on filenames instead of build ids.
  bool LocalizeUsingFilenames(const std::map<string, string>& filename_map);

  // Stores a list of unique filenames found in MMAP/MMAP2 events into
  // |filenames|.  Any existing data in |filenames| will be lost.
  void GetFilenames(std::vector<string>* filenames) const;
  void GetFilenamesAsSet(std::set<string>* filenames) const;

  // Uses build id events to populate |filenames_to_build_ids|.
  // Any existing data in |filenames_to_build_ids| will be lost.
  // Note:  A filename returned by GetFilenames need not be present in this map,
  // since there may be no build id event corresponding to the MMAP/MMAP2.
  void GetFilenamesToBuildIDs(
      std::map<string, string>* filenames_to_build_ids) const;

  static bool IsSupportedEventType(uint32_t type);

  // If a program using PerfReader calls events(), it could work with the
  // resulting events by importing kernel/perf_internals.h.  This would also
  // apply to other forms of data (attributes, event types, build ids, etc.)
  // However, there is no easy way to work with the sample info within events.
  // The following two methods have been added for this purpose.

  // Extracts from a perf event |event| info about the perf sample that
  // contains the event.  Stores info in |sample|.
  bool ReadPerfSampleInfo(const event_t& event,
                          struct perf_sample* sample) const;
  // Writes |sample| info back to a perf event |event|.
  bool WritePerfSampleInfo(const perf_sample& sample,
                           event_t* event) const;

  // Accessor funcs.
  const std::vector<PerfFileAttr>& attrs() const {
    return attrs_;
  }

  uint64_t sample_type() const {
    return sample_type_;
  }

  bool HaveEventNames() const {
    for (const auto& attr : attrs_) {
      if (attr.name.empty()) {
        return false;
      }
    }
    return true;
  }

  const std::vector<malloced_unique_ptr<event_t>>& events() const {
    return events_;
  }

  const std::vector<build_id_event*>& build_id_events() const {
    return build_id_events_;
  }

  const std::vector<char>& tracing_data() const {
    return tracing_data_;
  }

  uint64_t metadata_mask() const {
    return metadata_mask_;
  }

  const std::vector<PerfStringMetadata>& string_metadata() const {
    return string_metadata_;
  }

 protected:
  bool ReadHeader(DataReader* data);

  bool ReadAttrsSection(DataReader* data);
  bool ReadAttr(DataReader* data);
  bool ReadEventAttr(DataReader* data, perf_event_attr* attr);
  bool ReadUniqueIDs(DataReader* data, size_t num_ids, std::vector<u64>* ids);

  bool ReadEventTypesSection(DataReader* data);
  // if event_size == 0, then not in an event.
  bool ReadEventType(DataReader* data, size_t attr_idx, size_t event_size);

  bool ReadDataSection(DataReader* data);

  // Reads metadata in normal mode.
  bool ReadMetadata(DataReader* data);

  // The following functions read various types of metadata.
  bool ReadTracingMetadata(DataReader* data, size_t size);
  bool ReadBuildIDMetadata(DataReader* data, size_t size);
  // Reads contents of a build ID event or block beyond the header. Useful for
  // reading build IDs in piped mode, where the header must be read first in
  // order to determine that it is a build ID event.
  bool ReadBuildIDMetadataWithoutHeader(DataReader* data,
                                        const perf_event_header& header);

  bool ReadStringMetadata(DataReader* data, u32 type, size_t size);
  bool ReadUint32Metadata(DataReader* data, u32 type, size_t size);
  bool ReadUint64Metadata(DataReader* data, u32 type, size_t size);
  bool ReadCPUTopologyMetadata(DataReader* data, u32 type, size_t size);
  bool ReadNUMATopologyMetadata(DataReader* data, u32 type, size_t size);
  bool ReadEventDescMetadata(DataReader* data, u32 type, size_t size);

  // Read perf data from piped perf output data.
  bool ReadPipedData(DataReader* data);

  // Like WriteToPointer, but does not check if the buffer is large enough.
  bool WriteToPointerWithoutCheckingSize(char* buffer, size_t size);

  bool WriteHeader(DataWriter* data) const;
  bool WriteAttrs(DataWriter* data) const;
  bool WriteEventTypes(DataWriter* data) const;
  bool WriteData(DataWriter* data) const;
  bool WriteMetadata(DataWriter* data) const;

  // For writing the various types of metadata.
  bool WriteBuildIDMetadata(u32 type, DataWriter* data) const;
  bool WriteStringMetadata(u32 type, DataWriter* data) const;
  bool WriteUint32Metadata(u32 type, DataWriter* data) const;
  bool WriteUint64Metadata(u32 type, DataWriter* data) const;
  bool WriteEventDescMetadata(u32 type, DataWriter* data) const;
  bool WriteCPUTopologyMetadata(u32 type, DataWriter* data) const;
  bool WriteNUMATopologyMetadata(u32 type, DataWriter* data) const;

  // For reading event blocks within piped perf data.
  bool ReadAttrEventBlock(DataReader* data, size_t size);

  // Swaps byte order for non-header fields of the data structure pointed to by
  // |event|, if |is_cross_endian_| is true. Otherwise leaves the data the same.
  void MaybeSwapEventFields(event_t* event);

  // Returns the number of types of metadata stored and written to output data.
  size_t GetNumSupportedMetadata() const;

  // For computing the sizes of the various types of metadata.
  size_t GetBuildIDMetadataSize() const;
  size_t GetStringMetadataSize() const;
  size_t GetUint32MetadataSize() const;
  size_t GetUint64MetadataSize() const;
  size_t GetEventDescMetadataSize() const;
  size_t GetCPUTopologyMetadataSize() const;
  size_t GetNUMATopologyMetadataSize() const;

  // Returns true if we should write the number of strings for the string
  // metadata of type |type|.
  bool NeedsNumberOfStringData(u32 type) const;

  // Replaces existing filenames in MMAP/MMAP2 events based on |filename_map|.
  // This method does not change |build_id_events_|.
  bool LocalizeMMapFilenames(const std::map<string, string>& filename_map);

  std::vector<PerfFileAttr> attrs_;
  std::vector<malloced_unique_ptr<event_t>> events_;
  std::vector<build_id_event*> build_id_events_;
  std::vector<PerfStringMetadata> string_metadata_;
  std::vector<PerfUint32Metadata> uint32_metadata_;
  std::vector<PerfUint64Metadata> uint64_metadata_;
  PerfCPUTopologyMetadata cpu_topology_;
  std::vector<PerfNodeTopologyMetadata> numa_topology_;
  std::vector<char> tracing_data_;
  uint64_t sample_type_;
  uint64_t read_format_;
  uint64_t metadata_mask_;

  // Indicates that the perf data being read is from machine with a different
  // endianness than the current machine.
  bool is_cross_endian_;

 private:
  // The file header is either a normal header or a piped header.
  union {
    struct perf_file_header header_;
    struct perf_pipe_file_header piped_header_;
  };
  struct perf_file_header out_header_;

  DISALLOW_COPY_AND_ASSIGN(PerfReader);
};

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_PERF_READER_H_
