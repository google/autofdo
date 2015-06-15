// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromiumos-wide-profiling/perf_reader.h"

#include <byteswap.h>
#include <limits.h>

#include <algorithm>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

#include "base/logging.h"

#include "chromiumos-wide-profiling/limits.h"
#include "chromiumos-wide-profiling/quipper_string.h"
#include "chromiumos-wide-profiling/utils.h"

namespace quipper {

class BufferWithSize {
 public:
  BufferWithSize(void* buffer, size_t size)
      : buffer_(reinterpret_cast<char*>(buffer)),
        size_(size) {}

  // Writes raw data to buffer. Returns true if it managed to write |size|
  // bytes, and false if not.
  bool WriteData(const void* src, size_t offset, const size_t size) const {
    size_t write_size = size;
    if (offset + size >= size_)
      write_size = size_ - offset;
    memcpy(buffer_ + offset, src, write_size);

    if (write_size != size) {
      LOG(ERROR) << __func__ << ": offset=" << offset << ", size=" << size
                 << ", total_size=" << size_ << ", write_size=" << write_size;
    }
    return (write_size == size);
  }

  // Writes a string to the buffer. If the string length is smaller than |size|,
  // it will fill in the remainder of of the destination memory with zeroes.
  // If the string is longer than |size|, it will truncate the string, and will
  // not add a null terminator.
  // Returns true iff the expected number of bytes were written.
  bool WriteString(const string& str, size_t offset, const size_t size) const {
    // Make sure there is enough space left in the buffer.
    size_t available_size = std::min(size, size_ - offset);
    size_t string_write_size = std::min(str.size(), available_size);
    if (!WriteData(str.c_str(), offset, string_write_size))
      return false;

    // Add the padding, if any.
    available_size -= string_write_size;
    memset(buffer_ + offset + string_write_size, 0, available_size);
    return true;
  }

  size_t size() const {
    return size_;
  }

 private:
  char* buffer_;
  size_t size_;
};

// If the buffer is read-only, it is not sufficient to mark the previous struct
// as const, as this only means that the pointer cannot be changed, and says
// nothing about the contents of the buffer.  So, we need another struct.
class ConstBufferWithSize {
 public:
  ConstBufferWithSize(const void* data, size_t size)
      : data_(reinterpret_cast<const char*>(data)),
        size_(size) {}

  // Read raw data from the buffer. Returns true if it managed to read |size|
  // bytes, and false if not. Will not perform a partial copy.
  bool ReadData(void* dest, size_t offset, const size_t size) const {
    size_t read_size = size;
    if (offset + size > size_)
      return false;

    memcpy(dest, data_ + offset, read_size);
    return true;
  }

  // Read a string from the buffer. Returns true if it managed to read |size|
  // bytes (excluding null terminator), and false if not. The actual string
  // may be shorter than the number of bytes requested.
  bool ReadString(size_t offset, size_t size, string* str) const {
    if (offset + size > size_)
      return false;

    // The actual length of the string is allowed to be no more than |size|.
    // The raw string data doesn't require a null terminator. However, there
    // should be a null terminator if the string length is less than |size|, or
    // there would be no way to find the actual end of the string.
    size_t actual_length = strnlen(data_ + offset, size);
    *str = string(data_ + offset, actual_length);
    return true;
  }

  size_t size() const {
    return size_;
  }

 private:
  const char* data_;
  size_t size_;
};

namespace {

// The type of the number of string data, found in the command line metadata in
// the perf data file.
typedef u32 num_string_data_type;

// Types of the event desc fields that are not found in other structs.
typedef u32 event_desc_num_events;
typedef u32 event_desc_attr_size;
typedef u32 event_desc_num_unique_ids;

// The type of the number of nodes field in NUMA topology.
typedef u32 numa_topology_num_nodes_type;

// A mask that is applied to metadata_mask_ in order to get a mask for
// only the metadata supported by quipper.
const uint32_t kSupportedMetadataMask =
    1 << HEADER_TRACING_DATA |
    1 << HEADER_BUILD_ID |
    1 << HEADER_HOSTNAME |
    1 << HEADER_OSRELEASE |
    1 << HEADER_VERSION |
    1 << HEADER_ARCH |
    1 << HEADER_NRCPUS |
    1 << HEADER_CPUDESC |
    1 << HEADER_CPUID |
    1 << HEADER_TOTAL_MEM |
    1 << HEADER_CMDLINE |
    1 << HEADER_EVENT_DESC |
    1 << HEADER_CPU_TOPOLOGY |
    1 << HEADER_NUMA_TOPOLOGY |
    1 << HEADER_BRANCH_STACK;

// By default, the build ID event has PID = -1.
const uint32_t kDefaultBuildIDEventPid = static_cast<uint32_t>(-1);

// Eight bits in a byte.
size_t BytesToBits(size_t num_bytes) {
  return num_bytes * 8;
}

template <class T>
void ByteSwap(T* input) {
  switch (sizeof(T)) {
  case sizeof(uint8_t):
    LOG(WARNING) << "Attempting to byte swap on a single byte.";
    break;
  case sizeof(uint16_t):
    *input = bswap_16(*input);
    break;
  case sizeof(uint32_t):
    *input = bswap_32(*input);
    break;
  case sizeof(uint64_t):
    *input = bswap_64(*input);
    break;
  default:
    LOG(FATAL) << "Invalid size for byte swap: " << sizeof(T) << " bytes";
    break;
  }
}

u64 MaybeSwap(u64 value, bool swap) {
  if (swap)
    return bswap_64(value);
  return value;
}

u32 MaybeSwap(u32 value, bool swap) {
  if (swap)
    return bswap_32(value);
  return value;
}

u8 ReverseByte(u8 x) {
  x = (x & 0xf0) >> 4 | (x & 0x0f) << 4;  // exchange nibbles
  x = (x & 0xcc) >> 2 | (x & 0x33) << 2;  // exchange pairs
  x = (x & 0xaa) >> 1 | (x & 0x55) << 1;  // exchange neighbors
  return x;
}

// If field points to the start of a bitfield padded to len bytes, this
// performs an endian swap of the bitfield, assuming the compiler that produced
// it conforms to the same ABI (bitfield layout is not completely specified by
// the language).
void SwapBitfieldOfBits(u8* field, size_t len) {
  for (size_t i = 0; i < len; i++) {
    field[i] = ReverseByte(field[i]);
  }
}

// The code currently assumes that the compiler will not add any padding to the
// various structs.  These CHECKs make sure that this is true.
void CheckNoEventHeaderPadding() {
  perf_event_header header;
  CHECK_EQ(sizeof(header),
           sizeof(header.type) + sizeof(header.misc) + sizeof(header.size));
}

void CheckNoPerfEventAttrPadding() {
  perf_event_attr attr;
  CHECK_EQ(sizeof(attr),
           (reinterpret_cast<u64>(&attr.__reserved_2) -
            reinterpret_cast<u64>(&attr)) +
	   4 +
	   sizeof(attr.sample_regs_intr));
}

void CheckNoEventTypePadding() {
  perf_trace_event_type event_type;
  CHECK_EQ(sizeof(event_type),
           sizeof(event_type.event_id) + sizeof(event_type.name));
}

void CheckNoBuildIDEventPadding() {
  build_id_event event;
  CHECK_EQ(sizeof(event),
           sizeof(event.header.type) + sizeof(event.header.misc) +
           sizeof(event.header.size) + sizeof(event.pid) +
           sizeof(event.build_id));
}

// Creates/updates a build id event with |build_id| and |filename|.
// Passing "" to |build_id| or |filename| will leave the corresponding field
// unchanged (in which case |event| must be non-null).
// If |event| is null or is not large enough, a new event will be created.
// In this case, if |event| is non-null, it will be freed.
// Otherwise, updates the fields of the existing event.
// |new_misc| indicates kernel vs user space, and is only used to fill in the
// |header.misc| field of new events.
// In either case, returns a pointer to the event containing the updated data,
// or NULL in the case of a failure.
build_id_event* CreateOrUpdateBuildID(const string& build_id,
                                      const string& filename,
                                      uint16_t new_misc,
                                      build_id_event* event) {
  // When creating an event from scratch, build id and filename must be present.
  if (!event && (build_id.empty() || filename.empty()))
    return NULL;
  size_t new_len = GetUint64AlignedStringLength(
      filename.empty() ? event->filename : filename);

  // If event is null, or we don't have enough memory, allocate more memory, and
  // switch the new pointer with the existing pointer.
  size_t new_size = sizeof(*event) + new_len;
  if (!event || new_size > event->header.size) {
    build_id_event* new_event = CallocMemoryForBuildID(new_size);

    if (event) {
      // Copy over everything except the filename and free the event.
      // It is guaranteed that we are changing the filename - otherwise, the old
      // size and the new size would be equal.
      *new_event = *event;
      free(event);
    } else {
      // Fill in the fields appropriately.
      new_event->header.type = HEADER_BUILD_ID;
      new_event->header.misc = new_misc;
      new_event->pid = kDefaultBuildIDEventPid;
    }
    event = new_event;
  }

  // Here, event is the pointer to the build_id_event that we are keeping.
  // Update the event's size, build id, and filename.
  if (!build_id.empty() &&
      !StringToHex(build_id, event->build_id, arraysize(event->build_id))) {
    free(event);
    return NULL;
  }

  if (!filename.empty())
    CHECK_GT(snprintf(event->filename, new_len, "%s", filename.c_str()), 0);

  event->header.size = new_size;
  return event;
}

// Reads |size| bytes from |buffer| into |dest| and advances |src_offset|.
bool ReadDataFromBuffer(const ConstBufferWithSize& buffer,
                        size_t size,
                        const string& value_name,
                        size_t* src_offset,
                        void* dest) {
  if (!buffer.ReadData(dest, *src_offset, size)) {
    LOG(ERROR) << "Not enough bytes to read " << value_name
               << ". Requested " << size << " bytes";
    return false;
  }
  *src_offset += size;
  return true;
}

// Reads |size| bytes from |data| into |buffer| and advances |buffer_offset|.
bool WriteDataToBuffer(const void* data,
                       size_t size,
                       const string& value_name,
                       size_t* buffer_offset,
                       const BufferWithSize& buffer) {
  if (!buffer.WriteData(data, *buffer_offset, size)) {
    LOG(ERROR) << "No space in buffer to write " << value_name << ". "
               << "Requested " << size << " bytes, but only "
               << buffer.size() - *buffer_offset << " bytes remaining.";
    return false;
  }
  *buffer_offset += size;
  return true;
}

// Reads a CStringWithLength from |buffer| into |dest|, and advances the offset.
bool ReadStringFromBuffer(const ConstBufferWithSize& buffer,
                          bool is_cross_endian,
                          size_t* offset,
                          CStringWithLength* dest) {
  if (!ReadDataFromBuffer(buffer, sizeof(dest->len), "string length",
                          offset, &dest->len)) {
    return false;
  }
  if (is_cross_endian)
    ByteSwap(&dest->len);

  if (!buffer.ReadString(*offset, dest->len, &dest->str)) {
    LOG(ERROR) << "Not enough bytes to read string";
    return false;
  }
  *offset += dest->len;
  return true;
}

// Writes a CStringWithLength from |src| to |buffer|, and advances the offset.
bool WriteStringToBuffer(const CStringWithLength& src,
                         const BufferWithSize& buffer,
                         size_t* offset) {
  size_t final_offset = *offset + src.len + sizeof(src.len);
  if (buffer.size() < final_offset) {
    LOG(ERROR) << "Not enough space to write string";
    return false;
  }

  if (!WriteDataToBuffer(&src.len, sizeof(src.len),
                         "length of string metadata", offset, buffer) ||
      !buffer.WriteString(src.str, *offset, src.len)) {
    LOG(ERROR) << "Failed to write string to buffer.";
    return false;
  }
  *offset += src.len;
  return true;
}

// Read read info from perf data.  Corresponds to sample format type
// PERF_SAMPLE_READ.
const uint64_t* ReadReadInfo(const uint64_t* array,
                             bool swap_bytes,
                             uint64_t read_format,
                             struct perf_sample* sample) {
  if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
    sample->read.time_enabled = *array++;
  if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
    sample->read.time_running = *array++;
  if (read_format & PERF_FORMAT_ID)
    sample->read.id = *array++;

  if (swap_bytes) {
    ByteSwap(&sample->read.time_enabled);
    ByteSwap(&sample->read.time_running);
    ByteSwap(&sample->read.id);
  }

  return array;
}

// Read call chain info from perf data.  Corresponds to sample format type
// PERF_SAMPLE_CALLCHAIN.
const uint64_t* ReadCallchain(const uint64_t* array,
                              bool swap_bytes,
                              struct perf_sample* sample) {
  // Make sure there is no existing allocated memory in |sample->callchain|.
  CHECK_EQ(static_cast<void*>(NULL), sample->callchain);

  // The callgraph data consists of a uint64_t value |nr| followed by |nr|
  // addresses.
  uint64_t callchain_size = *array++;
  if (swap_bytes)
    ByteSwap(&callchain_size);
  struct ip_callchain* callchain =
      reinterpret_cast<struct ip_callchain*>(new uint64_t[callchain_size + 1]);
  callchain->nr = callchain_size;
  for (size_t i = 0; i < callchain_size; ++i) {
    callchain->ips[i] = *array++;
    if (swap_bytes)
      ByteSwap(&callchain->ips[i]);
  }
  sample->callchain = callchain;

  return array;
}

// Read raw info from perf data.  Corresponds to sample format type
// PERF_SAMPLE_RAW.
const uint64_t* ReadRawData(const uint64_t* array,
                            bool swap_bytes,
                            struct perf_sample* sample) {
  // First read the size.
  const uint32_t* ptr = reinterpret_cast<const uint32_t*>(array);
  sample->raw_size = *ptr++;
  if (swap_bytes)
    ByteSwap(&sample->raw_size);

  // Allocate space for and read the raw data bytes.
  sample->raw_data = new uint8_t[sample->raw_size];
  memcpy(sample->raw_data, ptr, sample->raw_size);

  // Determine the bytes that were read, and align to the next 64 bits.
  int bytes_read = AlignSize(sizeof(sample->raw_size) + sample->raw_size,
                             sizeof(uint64_t));
  array += bytes_read / sizeof(uint64_t);

  return array;
}

// Read call chain info from perf data.  Corresponds to sample format type
// PERF_SAMPLE_CALLCHAIN.
const uint64_t* ReadBranchStack(const uint64_t* array,
                                bool swap_bytes,
                                struct perf_sample* sample) {
  // Make sure there is no existing allocated memory in
  // |sample->branch_stack|.
  CHECK_EQ(static_cast<void*>(NULL), sample->branch_stack);

  // The branch stack data consists of a uint64_t value |nr| followed by |nr|
  // branch_entry structs.
  uint64_t branch_stack_size = *array++;
  if (swap_bytes)
    ByteSwap(&branch_stack_size);
  struct branch_stack* branch_stack =
      reinterpret_cast<struct branch_stack*>(
          new uint8_t[sizeof(uint64_t) +
                      branch_stack_size * sizeof(struct branch_entry)]);
  branch_stack->nr = branch_stack_size;
  for (size_t i = 0; i < branch_stack_size; ++i) {
    memcpy(&branch_stack->entries[i], array, sizeof(struct branch_entry));
    array += sizeof(struct branch_entry) / sizeof(*array);
    if (swap_bytes) {
      ByteSwap(&branch_stack->entries[i].from);
      ByteSwap(&branch_stack->entries[i].to);
    }
  }
  sample->branch_stack = branch_stack;

  return array;
}

size_t ReadPerfSampleFromData(const perf_event_type event_type,
                              const uint64_t* array,
                              const uint64_t sample_fields,
                              const uint64_t read_format,
                              bool swap_bytes,
                              struct perf_sample* sample) {
  const uint64_t* initial_array_ptr = array;

  union {
    uint32_t val32[sizeof(uint64_t) / sizeof(uint32_t)];
    uint64_t val64;
  };

  // See structure for PERF_RECORD_SAMPLE in kernel/perf_event.h
  // and compare sample_id when sample_id_all is set.

  // NB: For sample_id, sample_fields has already been masked to the set
  // of fields in that struct by GetSampleFieldsForEventType. That set
  // of fields is mostly in the same order as PERF_RECORD_SAMPLE, with
  // the exception of PERF_SAMPLE_IDENTIFIER.

  // PERF_SAMPLE_IDENTIFIER is in a different location depending on
  // if this is a SAMPLE event or the sample_id of another event.
  if (event_type == PERF_RECORD_SAMPLE) {
    // { u64                   id;       } && PERF_SAMPLE_IDENTIFIER
    if (sample_fields & PERF_SAMPLE_IDENTIFIER) {
      sample->id = MaybeSwap(*array++, swap_bytes);
    }
  }

  // { u64                   ip;       } && PERF_SAMPLE_IP
  if (sample_fields & PERF_SAMPLE_IP) {
    sample->ip = MaybeSwap(*array++, swap_bytes);
  }

  // { u32                   pid, tid; } && PERF_SAMPLE_TID
  if (sample_fields & PERF_SAMPLE_TID) {
    val64 = *array++;
    sample->pid = MaybeSwap(val32[0], swap_bytes);
    sample->tid = MaybeSwap(val32[1], swap_bytes);
  }

  // { u64                   time;     } && PERF_SAMPLE_TIME
  if (sample_fields & PERF_SAMPLE_TIME) {
    sample->time = MaybeSwap(*array++, swap_bytes);
  }

  // { u64                   addr;     } && PERF_SAMPLE_ADDR
  if (sample_fields & PERF_SAMPLE_ADDR) {
    sample->addr = MaybeSwap(*array++, swap_bytes);
  }

  // { u64                   id;       } && PERF_SAMPLE_ID
  if (sample_fields & PERF_SAMPLE_ID) {
    sample->id = MaybeSwap(*array++, swap_bytes);
  }

  // { u64                   stream_id;} && PERF_SAMPLE_STREAM_ID
  if (sample_fields & PERF_SAMPLE_STREAM_ID) {
    sample->stream_id = MaybeSwap(*array++, swap_bytes);
  }

  // { u32                   cpu, res; } && PERF_SAMPLE_CPU
  if (sample_fields & PERF_SAMPLE_CPU) {
    val64 = *array++;
    sample->cpu = MaybeSwap(val32[0], swap_bytes);
    // sample->res = MaybeSwap(*val32[1], swap_bytes);  // not implemented?
  }

  // This is the location of PERF_SAMPLE_IDENTIFIER in struct sample_id.
  if (event_type != PERF_RECORD_SAMPLE) {
    // { u64                   id;       } && PERF_SAMPLE_IDENTIFIER
    if (sample_fields & PERF_SAMPLE_IDENTIFIER) {
      sample->id = MaybeSwap(*array++, swap_bytes);
    }
  }

  //
  // The remaining fields are only in PERF_RECORD_SAMPLE
  //

  // { u64                   period;   } && PERF_SAMPLE_PERIOD
  if (sample_fields & PERF_SAMPLE_PERIOD) {
    sample->period = MaybeSwap(*array++, swap_bytes);
  }

  // { struct read_format    values;   } && PERF_SAMPLE_READ
  if (sample_fields & PERF_SAMPLE_READ) {
    // TODO(cwp-team): support grouped read info.
    if (read_format & PERF_FORMAT_GROUP)
      return 0;
    array = ReadReadInfo(array, swap_bytes, read_format, sample);
  }

  // { u64                   nr,
  //   u64                   ips[nr];  } && PERF_SAMPLE_CALLCHAIN
  if (sample_fields & PERF_SAMPLE_CALLCHAIN) {
    array = ReadCallchain(array, swap_bytes, sample);
  }

  // { u32                   size;
  //   char                  data[size];}&& PERF_SAMPLE_RAW
  if (sample_fields & PERF_SAMPLE_RAW) {
    array = ReadRawData(array, swap_bytes, sample);
  }

  // { u64                   nr;
  //   { u64 from, to, flags } lbr[nr];} && PERF_SAMPLE_BRANCH_STACK
  if (sample_fields & PERF_SAMPLE_BRANCH_STACK) {
    array = ReadBranchStack(array, swap_bytes, sample);
  }

  static const u64 kUnimplementedSampleFields =
      PERF_SAMPLE_REGS_USER  |
      PERF_SAMPLE_STACK_USER |
      PERF_SAMPLE_WEIGHT     |
      PERF_SAMPLE_DATA_SRC   |
      PERF_SAMPLE_TRANSACTION;

  if (sample_fields & kUnimplementedSampleFields) {
    LOG(WARNING) << "Unimplemented sample fields 0x"
                 << std::hex << (sample_fields & kUnimplementedSampleFields);
  }

  if (sample_fields & ~(PERF_SAMPLE_MAX-1)) {
    LOG(WARNING) << "Unrecognized sample fields 0x"
                 << std::hex << (sample_fields & ~(PERF_SAMPLE_MAX-1));
  }

  return (array - initial_array_ptr) * sizeof(uint64_t);
}

size_t WritePerfSampleToData(const perf_event_type event_type,
                             const struct perf_sample& sample,
                             const uint64_t sample_fields,
                             const uint64_t read_format,
                             uint64_t* array) {
  const uint64_t* initial_array_ptr = array;

  union {
    uint32_t val32[sizeof(uint64_t) / sizeof(uint32_t)];
    uint64_t val64;
  };

  // See notes at the top of ReadPerfSampleFromData regarding the structure
  // of PERF_RECORD_SAMPLE, sample_id, and PERF_SAMPLE_IDENTIFIER, as they
  // all apply here as well.

  // PERF_SAMPLE_IDENTIFIER is in a different location depending on
  // if this is a SAMPLE event or the sample_id of another event.
  if (event_type == PERF_RECORD_SAMPLE) {
    // { u64                   id;       } && PERF_SAMPLE_IDENTIFIER
    if (sample_fields & PERF_SAMPLE_IDENTIFIER) {
      *array++ = sample.id;
    }
  }

  // { u64                   ip;       } && PERF_SAMPLE_IP
  if (sample_fields & PERF_SAMPLE_IP) {
    *array++ = sample.ip;
  }

  // { u32                   pid, tid; } && PERF_SAMPLE_TID
  if (sample_fields & PERF_SAMPLE_TID) {
    val32[0] = sample.pid;
    val32[1] = sample.tid;
    *array++ = val64;
  }

  // { u64                   time;     } && PERF_SAMPLE_TIME
  if (sample_fields & PERF_SAMPLE_TIME) {
    *array++ = sample.time;
  }

  // { u64                   addr;     } && PERF_SAMPLE_ADDR
  if (sample_fields & PERF_SAMPLE_ADDR) {
    *array++ = sample.addr;
  }

  // { u64                   id;       } && PERF_SAMPLE_ID
  if (sample_fields & PERF_SAMPLE_ID) {
    *array++ = sample.id;
  }

  // { u64                   stream_id;} && PERF_SAMPLE_STREAM_ID
  if (sample_fields & PERF_SAMPLE_STREAM_ID) {
    *array++ = sample.stream_id;
  }

  // { u32                   cpu, res; } && PERF_SAMPLE_CPU
  if (sample_fields & PERF_SAMPLE_CPU) {
    val32[0] = sample.cpu;
    // val32[1] = sample.res;  // not implemented?
    val32[1] = 0;
    *array++ = val64;
  }

  // This is the location of PERF_SAMPLE_IDENTIFIER in struct sample_id.
  if (event_type != PERF_RECORD_SAMPLE) {
    // { u64                   id;       } && PERF_SAMPLE_IDENTIFIER
    if (sample_fields & PERF_SAMPLE_IDENTIFIER) {
      *array++ = sample.id;
    }
  }

  //
  // The remaining fields are only in PERF_RECORD_SAMPLE
  //

  // { u64                   period;   } && PERF_SAMPLE_PERIOD
  if (sample_fields & PERF_SAMPLE_PERIOD) {
    *array++ = sample.period;
  }

  // { struct read_format    values;   } && PERF_SAMPLE_READ
  if (sample_fields & PERF_SAMPLE_READ) {
    // TODO(cwp-team): support grouped read info.
    if (read_format & PERF_FORMAT_GROUP)
      return 0;
    if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
      *array++ = sample.read.time_enabled;
    if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
      *array++ = sample.read.time_running;
    if (read_format & PERF_FORMAT_ID)
      *array++ = sample.read.id;
  }

  // { u64                   nr,
  //   u64                   ips[nr];  } && PERF_SAMPLE_CALLCHAIN
  if (sample_fields & PERF_SAMPLE_CALLCHAIN) {
    if (!sample.callchain) {
      LOG(ERROR) << "Expecting callchain data, but none was found.";
    } else {
      *array++ = sample.callchain->nr;
      for (size_t i = 0; i < sample.callchain->nr; ++i)
        *array++ = sample.callchain->ips[i];
    }
  }

  // { u32                   size;
  //   char                  data[size];}&& PERF_SAMPLE_RAW
  if (sample_fields & PERF_SAMPLE_RAW) {
    uint32_t* ptr = reinterpret_cast<uint32_t*>(array);
    *ptr++ = sample.raw_size;
    memcpy(ptr, sample.raw_data, sample.raw_size);

    // Update the data read pointer after aligning to the next 64 bytes.
    int num_bytes = AlignSize(sizeof(sample.raw_size) + sample.raw_size,
                              sizeof(uint64_t));
    array += num_bytes / sizeof(uint64_t);
  }

  // { u64                   nr;
  //   { u64 from, to, flags } lbr[nr];} && PERF_SAMPLE_BRANCH_STACK
  if (sample_fields & PERF_SAMPLE_BRANCH_STACK) {
    if (!sample.branch_stack) {
      LOG(ERROR) << "Expecting branch stack data, but none was found.";
    } else {
      *array++ = sample.branch_stack->nr;
      for (size_t i = 0; i < sample.branch_stack->nr; ++i) {
        *array++ = sample.branch_stack->entries[i].from;
        *array++ = sample.branch_stack->entries[i].to;
        memcpy(array++, &sample.branch_stack->entries[i].flags,
               sizeof(uint64_t));
      }
    }
  }

  return (array - initial_array_ptr) * sizeof(uint64_t);
}

}  // namespace

PerfReader::~PerfReader() {
  // Free allocated memory.
  for (size_t i = 0; i < build_id_events_.size(); ++i)
    if (build_id_events_[i])
      free(build_id_events_[i]);
}

void PerfReader::PerfizeBuildIDString(string* build_id) {
  build_id->resize(kBuildIDStringLength, '0');
}

void PerfReader::UnperfizeBuildIDString(string* build_id) {
  const size_t kPaddingSize = 8;
  const string kBuildIDPadding = string(kPaddingSize, '0');

  // Remove kBuildIDPadding from the end of build_id until we cannot remove any
  // more, or removing more would cause the build id to be empty.
  while (build_id->size() > kPaddingSize &&
         build_id->substr(build_id->size() - kPaddingSize) == kBuildIDPadding) {
    build_id->resize(build_id->size() - kPaddingSize);
  }
}

bool PerfReader::ReadFile(const string& filename) {
  std::vector<char> data;
  if (!ReadFileToData(filename, &data))
    return false;
  return ReadFromVector(data);
}

bool PerfReader::ReadFromVector(const std::vector<char>& data) {
  return ReadFromPointer(&data[0], data.size());
}

bool PerfReader::ReadFromString(const string& str) {
  return ReadFromPointer(str.c_str(), str.size());
}

bool PerfReader::ReadFromPointer(const char* perf_data, size_t size) {
  const ConstBufferWithSize data(perf_data, size);

  if (data.size() == 0)
    return false;
  if (!ReadHeader(data))
    return false;

  // Check if it is normal perf data.
  if (header_.size == sizeof(header_)) {
    DLOG(INFO) << "Perf data is in normal format.";
    metadata_mask_ = header_.adds_features[0];
    if (!(metadata_mask_ & (1 << HEADER_EVENT_DESC))) {
      // Prefer to read attrs and event names from HEADER_EVENT_DESC metadata if
      // available. event_types section of perf.data is obsolete, but use it as
      // a fallback:
      if (!(ReadAttrs(data) && ReadEventTypes(data)))
        return false;
    }

    if (!(ReadData(data) && ReadMetadata(data)))
      return false;

    // We can construct HEADER_EVENT_DESC from attrs and event types.
    // NB: Can't set this before ReadMetadata(), or it may misread the metadata.
    metadata_mask_ |= (1 << HEADER_EVENT_DESC);
    return true;
  }

  // Otherwise it is piped data.
  if (piped_header_.size != sizeof(piped_header_)) {
    LOG(ERROR) << "Expecting piped data format, but header size "
               << piped_header_.size << " does not match expected size "
               << sizeof(piped_header_);
    return false;
  }

  return ReadPipedData(data);
}

bool PerfReader::WriteFile(const string& filename) {
  std::vector<char> data;
  return WriteToVector(&data) && WriteDataToFile(data, filename);
}

bool PerfReader::WriteToVector(std::vector<char>* data) {
  data->resize(GetSize());
  return WriteToPointerWithoutCheckingSize(&data->at(0), data->size());
}

bool PerfReader::WriteToString(string* str) {
  str->resize(GetSize());
  return WriteToPointerWithoutCheckingSize(&str->at(0), str->size());
}

bool PerfReader::WriteToPointer(char* buffer, size_t size) {
  size_t required_size = GetSize();
  if (size < required_size) {
    LOG(ERROR) << "Buffer is too small - buffer size is " << size
               << " and required size is " << required_size;
    return false;
  }
  return WriteToPointerWithoutCheckingSize(buffer, size);
}

bool PerfReader::WriteToPointerWithoutCheckingSize(char* buffer, size_t size) {
  BufferWithSize data(buffer, size);
  if (!WriteHeader(data) ||
      !WriteAttrs(data) ||
      !WriteEventTypes(data) ||
      !WriteData(data) ||
      !WriteMetadata(data)) {
    return false;
  }
  return true;
}

size_t PerfReader::GetSize() {
  // TODO(rohinmshah): This is not a good CHECK.  See TODO in perf_reader.h.
  CHECK(RegenerateHeader());

  size_t total_size = 0;
  total_size = 0;
  total_size += out_header_.size;
  total_size += out_header_.attrs.size;
  total_size += out_header_.event_types.size;
  total_size += out_header_.data.size;
  // Add the ID info, whose size is not explicitly included in the header.
  for (size_t i = 0; i < attrs_.size(); ++i)
    total_size += attrs_[i].ids.size() * sizeof(attrs_[i].ids[0]);

  // Additional info about metadata.  See WriteMetadata for explanation.
  total_size += (GetNumMetadata() + 1) * 2 * sizeof(u64);

  // Add the sizes of the various metadata.
  total_size += tracing_data_.size();
  total_size += GetBuildIDMetadataSize();
  total_size += GetStringMetadataSize();
  total_size += GetUint32MetadataSize();
  total_size += GetUint64MetadataSize();
  total_size += GetEventDescMetadataSize();
  total_size += GetCPUTopologyMetadataSize();
  total_size += GetNUMATopologyMetadataSize();
  return total_size;
}

bool PerfReader::RegenerateHeader() {
  // This is the order of the input perf file contents in normal mode:
  // 1. Header
  // 2. Attribute IDs (pointed to by attr.ids.offset)
  // 3. Attributes
  // 4. Event types
  // 5. Data
  // 6. Metadata

  // Compute offsets in the above order.
  CheckNoEventHeaderPadding();
  memset(&out_header_, 0, sizeof(out_header_));
  out_header_.magic = kPerfMagic;
  out_header_.size = sizeof(out_header_);
  out_header_.attr_size = sizeof(perf_file_attr);
  out_header_.attrs.size = out_header_.attr_size * attrs_.size();
  for (size_t i = 0; i < events_.size(); i++)
    out_header_.data.size += events_[i]->header.size;
  out_header_.event_types.size = HaveEventNames() ?
      (attrs_.size() * sizeof(perf_trace_event_type)) : 0;

  u64 current_offset = 0;
  current_offset += out_header_.size;
  for (size_t i = 0; i < attrs_.size(); i++)
    current_offset += sizeof(attrs_[i].ids[0]) * attrs_[i].ids.size();
  out_header_.attrs.offset = current_offset;
  current_offset += out_header_.attrs.size;
  out_header_.event_types.offset = current_offset;
  current_offset += out_header_.event_types.size;

  out_header_.data.offset = current_offset;

  // Construct the header feature bits.
  memset(&out_header_.adds_features, 0, sizeof(out_header_.adds_features));
  // The following code makes the assumption that all feature bits are in the
  // first word of |adds_features|.  If the perf data format changes and the
  // assumption is no longer valid, this CHECK will fail, at which point the
  // below code needs to be updated.  For now, sticking to that assumption keeps
  // the code simple.
  // This assumption is also used when reading metadata, so that code
  // will also have to be updated if this CHECK starts to fail.
  CHECK_LE(static_cast<size_t>(HEADER_LAST_FEATURE),
           BytesToBits(sizeof(out_header_.adds_features[0])));
  if (sample_type_ & PERF_SAMPLE_BRANCH_STACK)
    out_header_.adds_features[0] |= (1 << HEADER_BRANCH_STACK);
  out_header_.adds_features[0] |= metadata_mask_ & kSupportedMetadataMask;

  return true;
}

bool PerfReader::InjectBuildIDs(
    const std::map<string, string>& filenames_to_build_ids) {
  metadata_mask_ |= (1 << HEADER_BUILD_ID);
  std::set<string> updated_filenames;
  // Inject new build ID's for existing build ID events.
  for (size_t i = 0; i < build_id_events_.size(); ++i) {
    build_id_event* event = build_id_events_[i];
    string filename = event->filename;
    if (filenames_to_build_ids.find(filename) == filenames_to_build_ids.end())
      continue;

    string build_id = filenames_to_build_ids.at(filename);
    PerfizeBuildIDString(&build_id);
    // Changing a build id should always result in an update, never creation.
    CHECK_EQ(event, CreateOrUpdateBuildID(build_id, "", 0, event));
    updated_filenames.insert(filename);
  }

  // For files with no existing build ID events, create new build ID events.
  // This requires a lookup of all MMAP's to determine the |misc| field of each
  // build ID event.
  std::map<string, uint16_t> filename_to_misc;
  for (size_t i = 0; i < events_.size(); ++i) {
    const event_t& event = *events_[i];
    if (event.header.type == PERF_RECORD_MMAP)
      filename_to_misc[event.mmap.filename] = event.header.misc;
    if (event.header.type == PERF_RECORD_MMAP2)
      filename_to_misc[event.mmap2.filename] = event.header.misc;
  }

  std::map<string, string>::const_iterator it;
  for (it = filenames_to_build_ids.begin();
       it != filenames_to_build_ids.end();
       ++it) {
    const string& filename = it->first;
    if (updated_filenames.find(filename) != updated_filenames.end())
      continue;

    // Determine the misc field.
    uint16_t new_misc = PERF_RECORD_MISC_KERNEL;
    std::map<string, uint16_t>::const_iterator misc_iter =
        filename_to_misc.find(filename);
    if (misc_iter != filename_to_misc.end())
      new_misc = misc_iter->second;

    string build_id = it->second;
    PerfizeBuildIDString(&build_id);
    build_id_event* event =
        CreateOrUpdateBuildID(build_id, filename, new_misc, NULL);
    CHECK(event);
    build_id_events_.push_back(event);
  }

  return true;
}

bool PerfReader::Localize(
    const std::map<string, string>& build_ids_to_filenames) {
  std::map<string, string> perfized_build_ids_to_filenames;
  std::map<string, string>::const_iterator it;
  for (it = build_ids_to_filenames.begin();
       it != build_ids_to_filenames.end();
       ++it) {
    string build_id = it->first;
    PerfizeBuildIDString(&build_id);
    perfized_build_ids_to_filenames[build_id] = it->second;
  }

  std::map<string, string> filename_map;
  for (size_t i = 0; i < build_id_events_.size(); ++i) {
    build_id_event* event = build_id_events_[i];
    string build_id = HexToString(event->build_id, kBuildIDArraySize);
    if (perfized_build_ids_to_filenames.find(build_id) ==
        perfized_build_ids_to_filenames.end()) {
      continue;
    }

    string new_name = perfized_build_ids_to_filenames.at(build_id);
    filename_map[string(event->filename)] = new_name;
    build_id_event* new_event = CreateOrUpdateBuildID("", new_name, 0, event);
    CHECK(new_event);
    build_id_events_[i] = new_event;
  }

  LocalizeUsingFilenames(filename_map);
  return true;
}

bool PerfReader::LocalizeUsingFilenames(
    const std::map<string, string>& filename_map) {
  LocalizeMMapFilenames(filename_map);
  for (size_t i = 0; i < build_id_events_.size(); ++i) {
    build_id_event* event = build_id_events_[i];
    string old_name = event->filename;

    if (filename_map.find(event->filename) != filename_map.end()) {
      const string& new_name = filename_map.at(old_name);
      build_id_event* new_event = CreateOrUpdateBuildID("", new_name, 0, event);
      CHECK(new_event);
      build_id_events_[i] = new_event;
    }
  }
  return true;
}

void PerfReader::GetFilenames(std::vector<string>* filenames) const {
  std::set<string> filename_set;
  GetFilenamesAsSet(&filename_set);
  filenames->clear();
  filenames->insert(filenames->begin(), filename_set.begin(),
                    filename_set.end());
}

void PerfReader::GetFilenamesAsSet(std::set<string>* filenames) const {
  filenames->clear();
  for (size_t i = 0; i < events_.size(); ++i) {
    const event_t& event = *events_[i];
    if (event.header.type == PERF_RECORD_MMAP)
      filenames->insert(event.mmap.filename);
    if (event.header.type == PERF_RECORD_MMAP2)
      filenames->insert(event.mmap2.filename);
  }
}

void PerfReader::GetFilenamesToBuildIDs(
    std::map<string, string>* filenames_to_build_ids) const {
  filenames_to_build_ids->clear();
  for (size_t i = 0; i < build_id_events_.size(); ++i) {
    const build_id_event& event = *build_id_events_[i];
    string build_id = HexToString(event.build_id, kBuildIDArraySize);
    (*filenames_to_build_ids)[event.filename] = build_id;
  }
}

bool PerfReader::IsSupportedEventType(uint32_t type) {
  switch (type) {
  case PERF_RECORD_SAMPLE:
  case PERF_RECORD_MMAP:
  case PERF_RECORD_MMAP2:
  case PERF_RECORD_FORK:
  case PERF_RECORD_EXIT:
  case PERF_RECORD_COMM:
  case PERF_RECORD_LOST:
  case PERF_RECORD_THROTTLE:
  case PERF_RECORD_UNTHROTTLE:
    return true;
  case PERF_RECORD_READ:
  case PERF_RECORD_MAX:
    return false;
  default:
    LOG(FATAL) << "Unknown event type " << type;
    return false;
  }
}

bool PerfReader::ReadPerfSampleInfo(const event_t& event,
                                    struct perf_sample* sample) const {

  CHECK(sample);

  if (event.header.type >= PERF_RECORD_USER_TYPE_START && event.header.type < PERF_RECORD_HEADER_MAX) {
    memset(sample, 0, sizeof(perf_sample));
    return true;
  }

  if (!IsSupportedEventType(event.header.type)) {
    LOG(ERROR) << "Unsupported event type " << event.header.type;
    return false;
  }

  uint64_t sample_format = GetSampleFieldsForEventType(event.header.type,
                                                       sample_type_);
  uint64_t offset = GetPerfSampleDataOffset(event);
  size_t size_read = ReadPerfSampleFromData(
      static_cast<perf_event_type>(event.header.type),
      reinterpret_cast<const uint64_t*>(&event) + offset / sizeof(uint64_t),
      sample_format,
      read_format_,
      is_cross_endian_,
      sample);

  size_t expected_size = event.header.size - offset;
  if (size_read != expected_size) {
    LOG(ERROR) << "Read " << size_read << " bytes, expected "
               << expected_size << " bytes.";
  }

  return (size_read == expected_size);
}

bool PerfReader::WritePerfSampleInfo(const perf_sample& sample,
                                     event_t* event) const {
  CHECK(event);

  if (!IsSupportedEventType(event->header.type)) {
    LOG(ERROR) << "Unsupported event type " << event->header.type;
    return false;
  }

  uint64_t sample_format = GetSampleFieldsForEventType(event->header.type,
                                                       sample_type_);
  uint64_t offset = GetPerfSampleDataOffset(*event);

  size_t expected_size = event->header.size - offset;
  memset(reinterpret_cast<uint8_t*>(event) + offset, 0, expected_size);
  size_t size_written = WritePerfSampleToData(
      static_cast<perf_event_type>(event->header.type),
      sample,
      sample_format,
      read_format_,
      reinterpret_cast<uint64_t*>(event) + offset / sizeof(uint64_t));
  if (size_written != expected_size) {
    LOG(ERROR) << "Wrote " << size_written << " bytes, expected "
               << expected_size << " bytes.";
  }

  return (size_written == expected_size);
}

bool PerfReader::ReadHeader(const ConstBufferWithSize& data) {
  CheckNoEventHeaderPadding();
  size_t offset = 0;
  if (!ReadDataFromBuffer(data, sizeof(piped_header_), "header magic",
                          &offset, &piped_header_)) {
    return false;
  }
  if (piped_header_.magic != kPerfMagic &&
      piped_header_.magic != bswap_64(kPerfMagic)) {
    LOG(ERROR) << "Read wrong magic. Expected: 0x" << std::hex << kPerfMagic
               << " or 0x" << std::hex << bswap_64(kPerfMagic)
               << " Got: 0x" << std::hex << piped_header_.magic;
    return false;
  }
  is_cross_endian_ = (piped_header_.magic != kPerfMagic);
  if (is_cross_endian_)
    ByteSwap(&piped_header_.size);

  // Header can be a piped header.
  if (piped_header_.size == sizeof(piped_header_))
    return true;

  // Re-read full header
  offset = 0;
  if (!ReadDataFromBuffer(data, sizeof(header_), "header data",
                          &offset, &header_)) {
    return false;
  }
  if (is_cross_endian_)
    ByteSwap(&header_.size);

  return true;
}

bool PerfReader::ReadAttrs(const ConstBufferWithSize& data) {
  size_t num_attrs = header_.attrs.size / header_.attr_size;
  size_t offset = header_.attrs.offset;
  for (size_t i = 0; i < num_attrs; i++) {
    if (!ReadAttr(data, &offset))
      return false;
  }
  return true;
}

bool PerfReader::ReadAttr(const ConstBufferWithSize& data, size_t* offset) {
  PerfFileAttr attr;
  if (!ReadEventAttr(data, offset, &attr.attr))
    return false;

  perf_file_section ids;
  if (!ReadDataFromBuffer(data, sizeof(ids), "ID section info", offset, &ids))
    return false;
  if (is_cross_endian_) {
    ByteSwap(&ids.offset);
    ByteSwap(&ids.size);
  }

  size_t num_ids = ids.size / sizeof(decltype(attr.ids)::value_type);
  // Convert the offset from u64 to size_t.
  size_t ids_offset = ids.offset;
  if (!ReadUniqueIDs(data, num_ids, &ids_offset, &attr.ids))
    return false;
  attrs_.push_back(attr);
  return true;
}

u32 PerfReader::ReadPerfEventAttrSize(const ConstBufferWithSize& data,
                                      size_t attr_offset) {
  static_assert(std::is_same<decltype(perf_event_attr::size), u32>::value,
                "ReadPerfEventAttrSize return type should match "
                "perf_event_attr.size");
  u32 attr_size;
  size_t attr_size_offset = attr_offset + offsetof(perf_event_attr, size);
  if (!ReadDataFromBuffer(data, sizeof(perf_event_attr::size),
                          "attr.size", &attr_size_offset, &attr_size)) {
    return kUint32Max;
  }
  return MaybeSwap(attr_size, is_cross_endian_);
}

bool PerfReader::ReadEventAttr(const ConstBufferWithSize& data, size_t* offset,
                               perf_event_attr* attr) {
  CheckNoPerfEventAttrPadding();
  *attr = {0};

  // read just size first
  u32 attr_size = ReadPerfEventAttrSize(data, *offset);
  if (attr_size == kUint32Max) {
    return false;
  }

  // now read the the struct.
  if (!ReadDataFromBuffer(data, attr_size, "attribute", offset,
                          reinterpret_cast<char*>(attr))) {
    return false;
  }

  if (is_cross_endian_) {
    // Depending on attr->size, some of these might not have actually been
    // read. This is okay: they are zero.
    ByteSwap(&attr->type);
    ByteSwap(&attr->size);
    ByteSwap(&attr->config);
    ByteSwap(&attr->sample_period);
    ByteSwap(&attr->sample_type);
    ByteSwap(&attr->read_format);

    // NB: This will also reverse precise_ip : 2 as if it was two fields:
    auto *const bitfield_start = &attr->read_format + 1;
    SwapBitfieldOfBits(reinterpret_cast<u8*>(bitfield_start),
                       sizeof(u64));
    // ... So swap it back:
    const auto tmp = attr->precise_ip;
    attr->precise_ip = (tmp & 0x2) >> 1 | (tmp & 0x1) << 1;

    ByteSwap(&attr->wakeup_events);  // union with wakeup_watermark
    ByteSwap(&attr->bp_type);
    ByteSwap(&attr->bp_addr);        // union with config1
    ByteSwap(&attr->bp_len);         // union with config2
    ByteSwap(&attr->branch_sample_type);
    ByteSwap(&attr->sample_regs_user);
    ByteSwap(&attr->sample_stack_user);
  }

  CHECK_EQ(attr_size, attr->size);
  // The actual perf_event_attr data size might be different from the size of
  // the struct definition.  Check against perf_event_attr's |size| field.
  attr->size = sizeof(*attr);

  // Assign sample type if it hasn't been assigned, otherwise make sure all
  // subsequent attributes have the same sample type bits set.
  if (sample_type_ == 0) {
    sample_type_ = attr->sample_type;
  } else {
    CHECK_EQ(sample_type_, attr->sample_type)
        << "Event type sample format does not match sample format of other "
        << "event type.";
  }

  if (read_format_ == 0) {
    read_format_ = attr->read_format;
  } else {
    CHECK_EQ(read_format_, attr->read_format)
        << "Event type read format does not match read format of other event "
        << "types.";
  }

  return true;
}

bool PerfReader::ReadUniqueIDs(const ConstBufferWithSize& data, size_t num_ids,
                               size_t* offset, std::vector<u64>* ids) {
  ids->resize(num_ids);
  for (size_t j = 0; j < num_ids; j++) {
    if (!ReadDataFromBuffer(data, sizeof(ids->at(j)), "ID", offset,
                            &ids->at(j))) {
      return false;
    }
    if (is_cross_endian_)
      ByteSwap(&ids->at(j));
  }
  return true;
}

bool PerfReader::ReadEventTypes(const ConstBufferWithSize& data) {
  size_t num_event_types = header_.event_types.size /
      sizeof(struct perf_trace_event_type);
  if (num_event_types == 0) {
    // Not available.
    return true;
  }
  CHECK_EQ(attrs_.size(), num_event_types);
  CHECK_EQ(sizeof(perf_trace_event_type) * num_event_types,
           header_.event_types.size);
  size_t offset = header_.event_types.offset;
  for (size_t i = 0; i < num_event_types; ++i) {
    if (!ReadEventType(data, i, 0, &offset))
      return false;
  }
  return true;
}

bool PerfReader::ReadEventType(const ConstBufferWithSize& data,
                               size_t attr_idx, size_t event_size,
                               size_t* offset) {
  CheckNoEventTypePadding();
  decltype(perf_trace_event_type::event_id) event_id;

  if (!ReadDataFromBuffer(data, sizeof(event_id), "event id",
                          offset, &event_id)) {
    return false;
  }
  size_t event_name_len;
  if (event_size == 0) {  // Not in an event.
    event_name_len = sizeof(perf_trace_event_type::name);
  } else {
    event_name_len =
        event_size - sizeof(perf_event_header) - sizeof(event_id);
  }
  string event_name;
  if (!data.ReadString(*offset, event_name_len, &event_name)) {
    LOG(ERROR) << "Not enough data left in data to read event name.";
    return false;
  }
  *offset += event_name_len;

  if (attr_idx >= attrs_.size()) {
    LOG(ERROR) << "Too many event types, or attrs not read yet!";
    return false;
  }
  if (event_id != attrs_[attr_idx].attr.config) {
    LOG(ERROR) << "event_id for perf_trace_event_type does not match "
                  "attr.config";
    return false;
  }
  attrs_[attr_idx].name = event_name;

  return true;
}

bool PerfReader::ReadData(const ConstBufferWithSize& data) {
  u64 data_remaining_bytes = header_.data.size;
  size_t offset = header_.data.offset;
  while (data_remaining_bytes != 0) {
    if (data.size() < offset) {
      LOG(ERROR) << "Not enough data to read a perf event.";
      return false;
    }

    perf_event_header header;
    if (!data.ReadData(&header, offset, sizeof(header))) {
      LOG(ERROR) << "Not enough data to read perf event header.";
      return false;
    }
    if (is_cross_endian_) {
      ByteSwap(&header.type);
      ByteSwap(&header.misc);
      ByteSwap(&header.size);
    }

    // Allocate memory for event and copy over the header.
    malloced_unique_ptr<event_t> event(CallocMemoryForEvent(header.size));
    event->header = header;

    // Read the rest of the event data.
    data.ReadData(&event->header + 1, offset + sizeof(header),
                  header.size - sizeof(header));
    MaybeSwapEventFields(event.get());
    events_.push_back(std::move(event));

    data_remaining_bytes -= header.size;
    offset += header.size;
  }

  DLOG(INFO) << "Number of events stored: "<< events_.size();
  return true;
}

bool PerfReader::ReadMetadata(const ConstBufferWithSize& data) {
  size_t offset = header_.data.offset + header_.data.size;

  for (u32 type = HEADER_FIRST_FEATURE; type != HEADER_LAST_FEATURE; ++type) {
    if ((metadata_mask_ & (1 << type)) == 0)
      continue;

    if (data.size() < offset) {
      LOG(ERROR) << "Not enough data to read offset and size of metadata.";
      return false;
    }

    u64 metadata_offset, metadata_size;
    if (!ReadDataFromBuffer(data, sizeof(metadata_offset), "metadata offset",
                            &offset, &metadata_offset) ||
        !ReadDataFromBuffer(data, sizeof(metadata_size), "metadata size",
                            &offset, &metadata_size)) {
      return false;
    }

    if (data.size() < metadata_offset + metadata_size) {
      LOG(ERROR) << "Not enough data to read metadata.";
      return false;
    }

    switch (type) {
    case HEADER_TRACING_DATA:
      if (!ReadTracingMetadata(data, metadata_offset, metadata_size)) {
        return false;
      }
      break;
    case HEADER_BUILD_ID:
      if (!ReadBuildIDMetadata(data, type, metadata_offset, metadata_size))
        return false;
      break;
    case HEADER_HOSTNAME:
    case HEADER_OSRELEASE:
    case HEADER_VERSION:
    case HEADER_ARCH:
    case HEADER_CPUDESC:
    case HEADER_CPUID:
    case HEADER_CMDLINE:
      if (!ReadStringMetadata(data, type, metadata_offset, metadata_size))
        return false;
      break;
    case HEADER_NRCPUS:
      if (!ReadUint32Metadata(data, type, metadata_offset, metadata_size))
        return false;
      break;
    case HEADER_TOTAL_MEM:
      if (!ReadUint64Metadata(data, type, metadata_offset, metadata_size))
        return false;
      break;
    case HEADER_EVENT_DESC:
      if (!ReadEventDescMetadata(data, type, metadata_offset, metadata_size))
        return false;
      break;
    case HEADER_CPU_TOPOLOGY:
      if (!ReadCPUTopologyMetadata(data, type, metadata_offset, metadata_size))
        return false;
      break;
    case HEADER_NUMA_TOPOLOGY:
      if (!ReadNUMATopologyMetadata(data, type, metadata_offset, metadata_size))
        return false;
      break;
    case HEADER_BRANCH_STACK:
      continue;
    default: LOG(INFO) << "Unsupported metadata type: " << type;
      break;
    }
  }

  return true;
}

bool PerfReader::ReadBuildIDMetadata(const ConstBufferWithSize& data, u32 type,
                                     size_t offset, size_t size) {
  CheckNoBuildIDEventPadding();
  while (size > 0) {
    // Make sure there is enough data for everything but the filename.
    perf_event_header build_id_header;
    if (!data.ReadData(&build_id_header, offset, sizeof(build_id_header))) {
      LOG(ERROR) << "Not enough bytes to read build ID header";
      return false;
    }
    u16 event_size = build_id_header.size;
    if (is_cross_endian_)
      ByteSwap(&event_size);

    // Allocate memory for the event.
    malloced_unique_ptr<build_id_event> event(
        CallocMemoryForBuildID(event_size));

    // Make sure there is enough data for the rest of the event.
    if (!ReadDataFromBuffer(data, event_size, "build id event",
                            &offset, event.get())) {
      LOG(ERROR) << "Not enough bytes to read build id event";
      return false;
    }
    if (is_cross_endian_) {
      ByteSwap(&event->header.type);
      ByteSwap(&event->header.misc);
      ByteSwap(&event->header.size);
      ByteSwap(&event->pid);
    }
    size -= event_size;

    // Perf tends to use more space than necessary, so fix the size.
    event->header.size =
        sizeof(*event) + GetUint64AlignedStringLength(event->filename);
    // TODO(sque): Convert |build_id_events_| to a vector of
    // malloced_unique_ptrs, so this can be done using std::move().
    build_id_events_.push_back(event.release());
  }

  return true;
}

bool PerfReader::ReadStringMetadata(const ConstBufferWithSize& data, u32 type,
                                    size_t offset, size_t size) {
  PerfStringMetadata str_data;
  str_data.type = type;

  size_t start_offset = offset;
  // Skip the number of string data if it is present.
  if (NeedsNumberOfStringData(type))
    offset += sizeof(num_string_data_type);

  while ((offset - start_offset) < size) {
    CStringWithLength single_string;
    if (!ReadStringFromBuffer(data, is_cross_endian_, &offset, &single_string))
      return false;
    str_data.data.push_back(single_string);
  }

  string_metadata_.push_back(str_data);
  return true;
}

bool PerfReader::ReadUint32Metadata(const ConstBufferWithSize& data, u32 type,
                                    size_t offset, size_t size) {
  PerfUint32Metadata uint32_data;
  uint32_data.type = type;

  size_t start_offset = offset;
  while (size > offset - start_offset) {
    uint32_t item;
    if (!ReadDataFromBuffer(data, sizeof(item), "uint32_t data", &offset,
                            &item))
      return false;

    if (is_cross_endian_)
      ByteSwap(&item);

    uint32_data.data.push_back(item);
  }

  uint32_metadata_.push_back(uint32_data);
  return true;
}

bool PerfReader::ReadUint64Metadata(const ConstBufferWithSize& data, u32 type,
                                    size_t offset, size_t size) {
  PerfUint64Metadata uint64_data;
  uint64_data.type = type;

  size_t start_offset = offset;
  while (size > offset - start_offset) {
    uint64_t item;
    if (!ReadDataFromBuffer(data, sizeof(item), "uint64_t data", &offset,
                            &item))
      return false;

    if (is_cross_endian_)
      ByteSwap(&item);

    uint64_data.data.push_back(item);
  }

  uint64_metadata_.push_back(uint64_data);
  return true;
}

bool PerfReader::ReadEventDescMetadata(
    const ConstBufferWithSize& data, u32 type, size_t offset, size_t size) {
  // Structure:
  // u32 nr_events
  // u32 sizeof(perf_event_attr)
  // foreach event (nr_events):
  //   struct perf_event_attr
  //   u32 nr_ids
  //   event name (len & string, 64-bit padded)
  //   u64 ids[nr_ids]

  u32 nr_events;
  if (!ReadDataFromBuffer(data, sizeof(nr_events), "event_desc nr_events",
                          &offset, &nr_events)) {
    return false;
  }

  u32 attr_size;
  if (!ReadDataFromBuffer(data, sizeof(attr_size), "event_desc attr_size",
                          &offset, &attr_size)) {
    return false;
  }

  CHECK_LE(attr_size, sizeof(perf_event_attr));

  attrs_.clear();
  attrs_.resize(nr_events);

  for (u32 i = 0; i < nr_events; i++) {
    if (!ReadEventAttr(data, &offset, &attrs_[i].attr)) {
      return false;
    }

    u32 nr_ids;
    if (!ReadDataFromBuffer(data, sizeof(nr_ids), "event_desc nr_ids",
                            &offset, &nr_ids)) {
      return false;
    }

    CStringWithLength event_name;
    if (!ReadStringFromBuffer(data, is_cross_endian_, &offset, &event_name)) {
      return false;
    }
    attrs_[i].name = event_name.str;

    std::vector<u64> &ids = attrs_[i].ids;
    ids.resize(nr_ids);
    size_t sizeof_ids = sizeof(ids[0]) * ids.size();
    if (!ReadDataFromBuffer(data, sizeof_ids, "event_desc ids",
                            &offset, ids.data())) {
      return false;
    }
    if (is_cross_endian_) {
      for (auto &id : attrs_[i].ids) {
        ByteSwap(&id);
      }
    }
  }
  return true;
}

bool PerfReader::ReadCPUTopologyMetadata(
    const ConstBufferWithSize& data, u32 type, size_t offset, size_t size) {
  num_siblings_type num_core_siblings;
  if (!ReadDataFromBuffer(data, sizeof(num_core_siblings), "num cores",
                          &offset, &num_core_siblings)) {
    return false;
  }
  if (is_cross_endian_)
    ByteSwap(&num_core_siblings);

  cpu_topology_.core_siblings.resize(num_core_siblings);
  for (size_t i = 0; i < num_core_siblings; ++i) {
    if (!ReadStringFromBuffer(data, is_cross_endian_, &offset,
                              &cpu_topology_.core_siblings[i])) {
      return false;
    }
  }

  num_siblings_type num_thread_siblings;
  if (!ReadDataFromBuffer(data, sizeof(num_thread_siblings), "num threads",
                          &offset, &num_thread_siblings)) {
    return false;
  }
  if (is_cross_endian_)
    ByteSwap(&num_thread_siblings);

  cpu_topology_.thread_siblings.resize(num_thread_siblings);
  for (size_t i = 0; i < num_thread_siblings; ++i) {
    if (!ReadStringFromBuffer(data, is_cross_endian_, &offset,
                              &cpu_topology_.thread_siblings[i])) {
      return false;
    }
  }

  return true;
}

bool PerfReader::ReadNUMATopologyMetadata(
    const ConstBufferWithSize& data, u32 type, size_t offset, size_t size) {
  numa_topology_num_nodes_type num_nodes;
  if (!ReadDataFromBuffer(data, sizeof(num_nodes), "num nodes",
                          &offset, &num_nodes)) {
    return false;
  }
  if (is_cross_endian_)
    ByteSwap(&num_nodes);

  for (size_t i = 0; i < num_nodes; ++i) {
    PerfNodeTopologyMetadata node;
    if (!ReadDataFromBuffer(data, sizeof(node.id), "node id",
                            &offset, &node.id) ||
        !ReadDataFromBuffer(data, sizeof(node.total_memory),
                            "node total memory", &offset,
                            &node.total_memory) ||
        !ReadDataFromBuffer(data, sizeof(node.free_memory),
                            "node free memory", &offset, &node.free_memory) ||
        !ReadStringFromBuffer(data, is_cross_endian_, &offset,
                              &node.cpu_list)) {
      return false;
    }
    if (is_cross_endian_) {
      ByteSwap(&node.id);
      ByteSwap(&node.total_memory);
      ByteSwap(&node.free_memory);
    }
    numa_topology_.push_back(node);
  }
  return true;
}

bool PerfReader::ReadTracingMetadata(
    const ConstBufferWithSize& data, size_t offset, size_t size) {
  size_t tracing_data_offset = offset;
  tracing_data_.resize(size);
  return ReadDataFromBuffer(data, tracing_data_.size(), "tracing_data",
                            &tracing_data_offset, tracing_data_.data());
}

bool PerfReader::ReadTracingMetadataEvent(
    const ConstBufferWithSize& data, size_t offset) {
  // TRACING_DATA's header.size is a lie. It is the size of only the event
  // struct. The size of the data is in the event struct, and followed
  // immediately by the tracing header data.

  // Make a copy of the event (but not the tracing data)
  tracing_data_event tracing_event;
  if (!data.ReadData(&tracing_event, offset, sizeof(tracing_event))) {
    LOG(ERROR) << "Not enough bytes left in data to read tracing event.";
    return false;
  }

  if (is_cross_endian_) {
    ByteSwap(&tracing_event.header.type);
    ByteSwap(&tracing_event.header.misc);
    ByteSwap(&tracing_event.header.size);
    ByteSwap(&tracing_event.size);
  }

  return ReadTracingMetadata(data, offset + tracing_event.header.size,
                             tracing_event.size);
}

bool PerfReader::ReadPipedData(const ConstBufferWithSize& data) {
  size_t offset = piped_header_.size;
  bool result = true;
  size_t event_type_count = 0;

  metadata_mask_ = 0;
  CheckNoEventHeaderPadding();

  while (offset < data.size() && result) {
    perf_event_header header;
    // Read the header and swap bytes if necessary.
    if (!data.ReadData(&header, offset, sizeof(header))) {
      LOG(ERROR) << "Not enough bytes left in data to read header.  Required: "
                 << sizeof(header) << " bytes.  Available: "
                 << data.size() - offset << " bytes.";
      return true;
    }

    if (is_cross_endian_) {
      ByteSwap(&header.type);
      ByteSwap(&header.misc);
      ByteSwap(&header.size);
    }

    if (header.size == 0) {
      // Avoid an infinite loop.
      LOG(ERROR) << "Event size is zero. Type: " << header.type;
      return false;
    }

    // Allocate space for an event struct based on the size in the header. Don't
    // blindly allocate the entire event_t because it is a variable-sized type
    // that may include data beyond what's nominally declared in its definition.
    malloced_unique_ptr<event_t> event(CallocMemoryForEvent(header.size));
    event->header = header;

    // Compute the offset and size of the post-header part of the event data.
    size_t new_offset = offset + sizeof(header);
    size_t size_without_header = header.size - sizeof(header);

    if (data.size() < offset + header.size) {
      LOG(ERROR) << "Not enough bytes to read piped event.  Required: "
                 << header.size << " bytes.  Available: "
                 << data.size() - offset << " bytes.";
      return true;
    }

    if (header.type < PERF_RECORD_MAX) {
      // Read the rest of the event data.
      if (!data.ReadData(&event->header + 1, new_offset, size_without_header))
        return false;
      MaybeSwapEventFields(event.get());
      events_.push_back(std::move(event));

      offset += header.size;
      continue;
    }

    switch (header.type) {
    case PERF_RECORD_FINISHED_ROUND:
      result = true;
      break;
    case PERF_RECORD_HEADER_ATTR:
      result = ReadAttrEventBlock(data, new_offset, size_without_header);
      break;
    case PERF_RECORD_HEADER_EVENT_TYPE:
      result = ReadEventType(data, event_type_count++, header.size,
                             &new_offset);
      break;
    case PERF_RECORD_HEADER_EVENT_DESC:
      metadata_mask_ |= (1 << HEADER_EVENT_DESC);
      result = ReadEventDescMetadata(data, HEADER_EVENT_DESC, new_offset,
                                     size_without_header);
      break;
    case PERF_RECORD_HEADER_TRACING_DATA:
      metadata_mask_ |= (1 << HEADER_TRACING_DATA);
      result = ReadTracingMetadataEvent(data, offset);
      offset += tracing_data_.size();  // header.size is added below.
      break;
    case PERF_RECORD_HEADER_BUILD_ID:
      metadata_mask_ |= (1 << HEADER_BUILD_ID);
      result = ReadBuildIDMetadata(data, HEADER_BUILD_ID, offset, header.size);
      break;
    case PERF_RECORD_HEADER_HOSTNAME:
      metadata_mask_ |= (1 << HEADER_HOSTNAME);
      result = ReadStringMetadata(data, HEADER_HOSTNAME, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_OSRELEASE:
      metadata_mask_ |= (1 << HEADER_OSRELEASE);
      result = ReadStringMetadata(data, HEADER_OSRELEASE, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_VERSION:
      metadata_mask_ |= (1 << HEADER_VERSION);
      result = ReadStringMetadata(data, HEADER_VERSION, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_ARCH:
      metadata_mask_ |= (1 << HEADER_ARCH);
      result = ReadStringMetadata(data, HEADER_ARCH, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_CPUDESC:
      metadata_mask_ |= (1 << HEADER_CPUDESC);
      result = ReadStringMetadata(data, HEADER_CPUDESC, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_CPUID:
      metadata_mask_ |= (1 << HEADER_CPUID);
      result = ReadStringMetadata(data, HEADER_CPUID, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_CMDLINE:
      metadata_mask_ |= (1 << HEADER_CMDLINE);
      result = ReadStringMetadata(data, HEADER_CMDLINE, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_NRCPUS:
      metadata_mask_ |= (1 << HEADER_NRCPUS);
      result = ReadUint32Metadata(data, HEADER_NRCPUS, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_TOTAL_MEM:
      metadata_mask_ |= (1 << HEADER_TOTAL_MEM);
      result = ReadUint64Metadata(data, HEADER_TOTAL_MEM, new_offset,
                                  size_without_header);
      break;
    case PERF_RECORD_HEADER_CPU_TOPOLOGY:
      metadata_mask_ |= (1 << HEADER_CPU_TOPOLOGY);
      result = ReadCPUTopologyMetadata(data, HEADER_CPU_TOPOLOGY, new_offset,
                                       size_without_header);
      break;
    case PERF_RECORD_HEADER_NUMA_TOPOLOGY:
      metadata_mask_ |= (1 << HEADER_NUMA_TOPOLOGY);
      result = ReadNUMATopologyMetadata(data, HEADER_NUMA_TOPOLOGY, new_offset,
                                        size_without_header);
      break;
    default:
      LOG(WARNING) << "Event type " << header.type
                   << " is not yet supported!";
      break;
    }
    offset += header.size;
  }

  if (!result) {
    return false;
  }

  // The PERF_RECORD_HEADER_EVENT_TYPE events are obsolete, but if present
  // and PERF_RECORD_HEADER_EVENT_DESC metadata events are not, we should use
  // them. Otherwise, we should use prefer the _EVENT_DESC data.
  if (!(metadata_mask_ & (1 << HEADER_EVENT_DESC)) &&
      event_type_count == attrs_.size()) {
    // We can construct HEADER_EVENT_DESC:
    metadata_mask_ |= (1 << HEADER_EVENT_DESC);
  }
  return result;
}

bool PerfReader::WriteHeader(const BufferWithSize& data) const {
  CheckNoEventHeaderPadding();
  size_t size = sizeof(out_header_);
  size_t offset = 0;
  return WriteDataToBuffer(&out_header_, size, "file header", &offset, data);
}

bool PerfReader::WriteAttrs(const BufferWithSize& data) const {
  CheckNoPerfEventAttrPadding();
  size_t offset = out_header_.attrs.offset;
  size_t id_offset = out_header_.size;

  for (size_t i = 0; i < attrs_.size(); i++) {
    const PerfFileAttr& attr = attrs_[i];
    struct perf_file_section ids;
    ids.offset = id_offset;
    ids.size = attr.ids.size() * sizeof(decltype(attr.ids)::value_type);

    for (size_t j = 0; j < attr.ids.size(); j++) {
      const uint64_t id = attr.ids[j];
      if (!WriteDataToBuffer(&id, sizeof(id), "ID info", &id_offset, data))
        return false;
    }

    if (!WriteDataToBuffer(&attr.attr, sizeof(attr.attr), "attribute",
                           &offset, data) ||
        !WriteDataToBuffer(&ids, sizeof(ids), "ID section", &offset, data)) {
      return false;
    }
  }
  return true;
}

bool PerfReader::WriteData(const BufferWithSize& data) const {
  size_t offset = out_header_.data.offset;
  for (size_t i = 0; i < events_.size(); ++i) {
    if (!WriteDataToBuffer(events_[i].get(), events_[i]->header.size,
                           "event data", &offset, data)) {
      return false;
    }
  }
  return true;
}

bool PerfReader::WriteMetadata(const BufferWithSize& data) const {
  size_t header_offset = out_header_.data.offset + out_header_.data.size;

  // Before writing the metadata, there is one header for each piece
  // of metadata, and one extra showing the end of the file.
  // Each header contains two 64-bit numbers (offset and size).
  size_t metadata_offset =
      header_offset + (GetNumMetadata() + 1) * 2 * sizeof(u64);

  for (u32 type = HEADER_FIRST_FEATURE; type != HEADER_LAST_FEATURE; ++type) {
    if ((out_header_.adds_features[0] & (1 << type)) == 0)
      continue;

    u64 start_offset = metadata_offset;
    // Write actual metadata to address metadata_offset
    switch (type) {
    case HEADER_TRACING_DATA:
      if (!WriteDataToBuffer(tracing_data_.data(), tracing_data_.size(),
                             "tracing data", &metadata_offset, data)) {
        return false;
      }
      break;
    case HEADER_BUILD_ID:
      if (!WriteBuildIDMetadata(type, &metadata_offset, data))
        return false;
      break;
    case HEADER_HOSTNAME:
    case HEADER_OSRELEASE:
    case HEADER_VERSION:
    case HEADER_ARCH:
    case HEADER_CPUDESC:
    case HEADER_CPUID:
    case HEADER_CMDLINE:
      if (!WriteStringMetadata(type, &metadata_offset, data))
        return false;
      break;
    case HEADER_NRCPUS:
      if (!WriteUint32Metadata(type, &metadata_offset, data))
        return false;
      break;
    case HEADER_TOTAL_MEM:
      if (!WriteUint64Metadata(type, &metadata_offset, data))
        return false;
      break;
    case HEADER_EVENT_DESC:
      if (!WriteEventDescMetadata(type, &metadata_offset, data))
        return false;
      break;
    case HEADER_CPU_TOPOLOGY:
      if (!WriteCPUTopologyMetadata(type, &metadata_offset, data))
        return false;
      break;
    case HEADER_NUMA_TOPOLOGY:
      if (!WriteNUMATopologyMetadata(type, &metadata_offset, data))
        return false;
      break;
    case HEADER_BRANCH_STACK:
      continue;
    default: LOG(ERROR) << "Unsupported metadata type: " << type;
      return false;
    }

    // Write metadata offset and size to address |header_offset|. This is the
    // metadata header that points to the metadata.
    u64 metadata_size = metadata_offset - start_offset;
    if (!WriteDataToBuffer(&start_offset, sizeof(start_offset),
                           "metadata offset", &header_offset, data) ||
        !WriteDataToBuffer(&metadata_size, sizeof(metadata_size),
                           "metadata size", &header_offset, data)) {
      return false;
    }
  }

  // Write the last entry - a pointer to the end of the file
  if (!WriteDataToBuffer(&metadata_offset, sizeof(metadata_offset),
                         "metadata offset", &header_offset, data)) {
    return false;
  }

  return true;
}

bool PerfReader::WriteBuildIDMetadata(u32 type, size_t* offset,
                                      const BufferWithSize& data) const {
  CheckNoBuildIDEventPadding();
  for (size_t i = 0; i < build_id_events_.size(); ++i) {
    const build_id_event* event = build_id_events_[i];
    if (!WriteDataToBuffer(event, event->header.size, "Build ID metadata",
                           offset, data)) {
      return false;
    }
  }
  return true;
}

bool PerfReader::WriteStringMetadata(u32 type, size_t* offset,
                                     const BufferWithSize& data) const {
  for (size_t i = 0; i < string_metadata_.size(); ++i) {
    const PerfStringMetadata& str_data = string_metadata_[i];
    if (str_data.type == type) {
      num_string_data_type num_strings = str_data.data.size();
      if (NeedsNumberOfStringData(type) &&
          !WriteDataToBuffer(&num_strings, sizeof(num_strings),
                             "number of string metadata", offset, data)) {
        return false;
      }

      for (size_t j = 0; j < num_strings; ++j) {
        const CStringWithLength& single_string = str_data.data[j];
        if (!WriteStringToBuffer(single_string, data, offset))
          return false;
      }

      return true;
    }
  }
  LOG(ERROR) << "String metadata of type " << type << " not present";
  return false;
}

bool PerfReader::WriteUint32Metadata(u32 type, size_t* offset,
                                     const BufferWithSize& data) const {
  for (size_t i = 0; i < uint32_metadata_.size(); ++i) {
    const PerfUint32Metadata& uint32_data = uint32_metadata_[i];
    if (uint32_data.type == type) {
      const std::vector<uint32_t>& int_vector = uint32_data.data;

      for (size_t j = 0; j < int_vector.size(); ++j) {
        if (!WriteDataToBuffer(&int_vector[j], sizeof(int_vector[j]),
                               "uint32_t metadata", offset, data)) {
          return false;
        }
      }

      return true;
    }
  }
  LOG(ERROR) << "Uint32 metadata of type " << type << " not present";
  return false;
}

bool PerfReader::WriteUint64Metadata(u32 type, size_t* offset,
                                     const BufferWithSize& data) const {
  for (size_t i = 0; i < uint64_metadata_.size(); ++i) {
    const PerfUint64Metadata& uint64_data = uint64_metadata_[i];
    if (uint64_data.type == type) {
      const std::vector<uint64_t>& int_vector = uint64_data.data;

      for (size_t j = 0; j < int_vector.size(); ++j) {
        if (!WriteDataToBuffer(&int_vector[j], sizeof(int_vector[j]),
                               "uint64_t metadata", offset, data)) {
          return false;
        }
      }

      return true;
    }
  }
  LOG(ERROR) << "Uint64 metadata of type " << type << " not present";
  return false;
}

bool PerfReader::WriteEventDescMetadata(u32 type, size_t* offset,
                                        const BufferWithSize& data) const {
  CheckNoPerfEventAttrPadding();

  event_desc_num_events num_events = attrs_.size();
  if (!WriteDataToBuffer(&num_events, sizeof(num_events),
                         "event_desc num_events", offset, data)) {
    return false;
  }
  event_desc_attr_size attr_size = sizeof(perf_event_attr);
  if (!WriteDataToBuffer(&attr_size, sizeof(attr_size),
                         "event_desc attr_size", offset, data)) {
    return false;
  }

  for (size_t i = 0; i < num_events; ++i) {
    const PerfFileAttr& attr = attrs_[i];
    if (!WriteDataToBuffer(&attr.attr, sizeof(attr.attr),
                           "event_desc attribute", offset, data)) {
      return false;
    }

    event_desc_num_unique_ids num_ids = attr.ids.size();
    if (!WriteDataToBuffer(&num_ids, sizeof(num_ids),
                           "event_desc num_unique_ids", offset, data)) {
      return false;
    }

    CStringWithLength container;
    container.len = GetUint64AlignedStringLength(attr.name);
    container.str = attr.name;
    if (!WriteStringToBuffer(container, data, offset))
      return false;

    if (!WriteDataToBuffer(attr.ids.data(), num_ids * sizeof(attr.ids[0]),
                           "event_desc unique_ids", offset, data)) {
      return false;
    }
  }
  return true;
}

bool PerfReader::WriteCPUTopologyMetadata(u32 type, size_t* offset,
                                          const BufferWithSize& data) const {
  const std::vector<CStringWithLength>& cores = cpu_topology_.core_siblings;
  num_siblings_type num_cores = cores.size();
  if (!WriteDataToBuffer(&num_cores, sizeof(num_cores), "num cores",
                         offset, data)) {
    return false;
  }
  for (size_t i = 0; i < num_cores; ++i) {
    if (!WriteStringToBuffer(cores[i], data, offset))
      return false;
  }

  const std::vector<CStringWithLength>& threads = cpu_topology_.thread_siblings;
  num_siblings_type num_threads = threads.size();
  if (!WriteDataToBuffer(&num_threads, sizeof(num_threads), "num threads",
                         offset, data)) {
    return false;
  }
  for (size_t i = 0; i < num_threads; ++i) {
    if (!WriteStringToBuffer(threads[i], data, offset))
      return false;
  }

  return true;
}

bool PerfReader::WriteNUMATopologyMetadata(u32 type, size_t* offset,
                                           const BufferWithSize& data) const {
  numa_topology_num_nodes_type num_nodes = numa_topology_.size();
  if (!WriteDataToBuffer(&num_nodes, sizeof(num_nodes), "num nodes",
                         offset, data)) {
    return false;
  }

  for (size_t i = 0; i < num_nodes; ++i) {
    const PerfNodeTopologyMetadata& node = numa_topology_[i];
    if (!WriteDataToBuffer(&node.id, sizeof(node.id), "node id",
                           offset, data) ||
        !WriteDataToBuffer(&node.total_memory, sizeof(node.total_memory),
                           "node total memory", offset, data) ||
        !WriteDataToBuffer(&node.free_memory, sizeof(node.free_memory),
                           "node free memory", offset, data) ||
        !WriteStringToBuffer(node.cpu_list, data, offset)) {
      return false;
    }
  }
  return true;
}

bool PerfReader::WriteEventTypes(const BufferWithSize& data) const {
  CheckNoEventTypePadding();
  size_t offset = out_header_.event_types.offset;
  const size_t size = out_header_.event_types.size;
  if (size == 0) {
    return true;
  }
  for (size_t i = 0; i < attrs_.size(); ++i) {
    struct perf_trace_event_type event_type = {0};
    event_type.event_id = attrs_[i].attr.config;
    const string& name = attrs_[i].name;
    memcpy(&event_type.name, name.data(),
           std::min(name.size(), sizeof(event_type.name)));
    if (!WriteDataToBuffer(&event_type, sizeof(event_type), "event type info",
                           &offset, data)) {
      return false;
    }
  }
  CHECK_EQ(size, offset - out_header_.event_types.offset);
  return true;
}

bool PerfReader::ReadAttrEventBlock(const ConstBufferWithSize& data,
                                    size_t offset, size_t size) {
  const size_t initial_offset = offset;
  PerfFileAttr attr;
  if (!ReadEventAttr(data, &offset, &attr.attr))
    return false;

  // attr.attr.size has been upgraded to the current size of perf_event_attr.
  const size_t actual_attr_size = offset - initial_offset;

  const size_t num_ids =
      (size - actual_attr_size) / sizeof(decltype(attr.ids)::value_type);
  if (!ReadUniqueIDs(data, num_ids, &offset, &attr.ids))
    return false;

  // Event types are found many times in the perf data file.
  // Only add this event type if it is not already present.
  for (size_t i = 0; i < attrs_.size(); ++i) {
    if (attrs_[i].ids[0] == attr.ids[0])
      return true;
  }
  attrs_.push_back(attr);
  return true;
}

void PerfReader::MaybeSwapEventFields(event_t* event) {
  uint32_t type = event->header.type;
  if (is_cross_endian_) {
    switch (type) {
    case PERF_RECORD_SAMPLE:
      break;
    case PERF_RECORD_MMAP:
      ByteSwap(&event->mmap.pid);
      ByteSwap(&event->mmap.tid);
      ByteSwap(&event->mmap.start);
      ByteSwap(&event->mmap.len);
      ByteSwap(&event->mmap.pgoff);
      break;
    case PERF_RECORD_MMAP2:
      ByteSwap(&event->mmap2.pid);
      ByteSwap(&event->mmap2.tid);
      ByteSwap(&event->mmap2.start);
      ByteSwap(&event->mmap2.len);
      ByteSwap(&event->mmap2.pgoff);
      ByteSwap(&event->mmap2.maj);
      ByteSwap(&event->mmap2.min);
      ByteSwap(&event->mmap2.ino);
      ByteSwap(&event->mmap2.ino_generation);
      ByteSwap(&event->mmap2.prot);
      ByteSwap(&event->mmap2.flags);
      break;
    case PERF_RECORD_FORK:
    case PERF_RECORD_EXIT:
      ByteSwap(&event->fork.pid);
      ByteSwap(&event->fork.tid);
      ByteSwap(&event->fork.ppid);
      ByteSwap(&event->fork.ptid);
      break;
    case PERF_RECORD_COMM:
      ByteSwap(&event->comm.pid);
      ByteSwap(&event->comm.tid);
      break;
    case PERF_RECORD_LOST:
      ByteSwap(&event->lost.id);
      ByteSwap(&event->lost.lost);
      break;
    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
      ByteSwap(&event->throttle.time);
      ByteSwap(&event->throttle.id);
      ByteSwap(&event->throttle.stream_id);
      break;
    case PERF_RECORD_READ:
      ByteSwap(&event->read.pid);
      ByteSwap(&event->read.tid);
      ByteSwap(&event->read.value);
      ByteSwap(&event->read.time_enabled);
      ByteSwap(&event->read.time_running);
      ByteSwap(&event->read.id);
      break;
    default:
      LOG(FATAL) << "Unknown event type: " << type;
    }
  }
}

size_t PerfReader::GetNumMetadata() const {
  // This is just the number of 1s in the binary representation of the metadata
  // mask.  However, make sure to only use supported metadata, and don't include
  // branch stack (since it doesn't have an entry in the metadata section).
  uint64_t new_mask = metadata_mask_;
  new_mask &= kSupportedMetadataMask & ~(1 << HEADER_BRANCH_STACK);
  std::bitset<sizeof(new_mask) * CHAR_BIT> bits(new_mask);
  return bits.count();
}

size_t PerfReader::GetEventDescMetadataSize() const {
  size_t size = 0;
  if (metadata_mask_ & (1 << HEADER_EVENT_DESC)) {
    size += sizeof(event_desc_num_events) + sizeof(event_desc_attr_size);
    CStringWithLength dummy;
    for (size_t i = 0; i < attrs_.size(); ++i) {
      size += sizeof(perf_event_attr) + sizeof(dummy.len);
      size += sizeof(event_desc_num_unique_ids);
      size += GetUint64AlignedStringLength(attrs_[i].name);
      size += attrs_[i].ids.size() * sizeof(attrs_[i].ids[0]);
    }
  }
  return size;
}

size_t PerfReader::GetBuildIDMetadataSize() const {
  size_t size = 0;
  for (size_t i = 0; i < build_id_events_.size(); ++i)
    size += build_id_events_[i]->header.size;
  return size;
}

size_t PerfReader::GetStringMetadataSize() const {
  size_t size = 0;
  for (size_t i = 0; i < string_metadata_.size(); ++i) {
    const PerfStringMetadata& metadata = string_metadata_[i];
    if (NeedsNumberOfStringData(metadata.type))
      size += sizeof(num_string_data_type);

    for (size_t j = 0; j < metadata.data.size(); ++j) {
      const CStringWithLength& str = metadata.data[j];
      size += sizeof(str.len) + str.len;
    }
  }
  return size;
}

size_t PerfReader::GetUint32MetadataSize() const {
  size_t size = 0;
  for (size_t i = 0; i < uint32_metadata_.size(); ++i) {
    const PerfUint32Metadata& metadata = uint32_metadata_[i];
    size += metadata.data.size() * sizeof(metadata.data[0]);
  }
  return size;
}

size_t PerfReader::GetUint64MetadataSize() const {
  size_t size = 0;
  for (size_t i = 0; i < uint64_metadata_.size(); ++i) {
    const PerfUint64Metadata& metadata = uint64_metadata_[i];
    size += metadata.data.size() * sizeof(metadata.data[0]);
  }
  return size;
}

size_t PerfReader::GetCPUTopologyMetadataSize() const {
  // Core siblings.
  size_t size = sizeof(num_siblings_type);
  for (size_t i = 0; i < cpu_topology_.core_siblings.size(); ++i) {
    const CStringWithLength& str = cpu_topology_.core_siblings[i];
    size += sizeof(str.len) + str.len;
  }

  // Thread siblings.
  size += sizeof(num_siblings_type);
  for (size_t i = 0; i < cpu_topology_.thread_siblings.size(); ++i) {
    const CStringWithLength& str = cpu_topology_.thread_siblings[i];
    size += sizeof(str.len) + str.len;
  }

  return size;
}

size_t PerfReader::GetNUMATopologyMetadataSize() const {
  size_t size = sizeof(numa_topology_num_nodes_type);
  for (size_t i = 0; i < numa_topology_.size(); ++i) {
    const PerfNodeTopologyMetadata& node = numa_topology_[i];
    size += sizeof(node.id);
    size += sizeof(node.total_memory) + sizeof(node.free_memory);
    size += sizeof(node.cpu_list.len) + node.cpu_list.len;
  }
  return size;
}

bool PerfReader::NeedsNumberOfStringData(u32 type) const {
  return type == HEADER_CMDLINE;
}

bool PerfReader::LocalizeMMapFilenames(
    const std::map<string, string>& filename_map) {
  // Search for mmap/mmap2 events for which the filename needs to be updated.
  for (size_t i = 0; i < events_.size(); ++i) {
    string filename;
    size_t size_of_fixed_event_parts;
    event_t* event = events_[i].get();
    if (event->header.type == PERF_RECORD_MMAP) {
      filename = string(event->mmap.filename);
      size_of_fixed_event_parts =
          sizeof(event->mmap) - sizeof(event->mmap.filename);
    } else if (event->header.type == PERF_RECORD_MMAP2) {
      filename = string(event->mmap2.filename);
      size_of_fixed_event_parts =
          sizeof(event->mmap2) - sizeof(event->mmap2.filename);
    } else {
      continue;
    }

    const auto it = filename_map.find(filename);
    if (it == filename_map.end())  // not found
      continue;

    const string& new_filename = it->second;
    size_t old_len = GetUint64AlignedStringLength(filename);
    size_t new_len = GetUint64AlignedStringLength(new_filename);
    size_t old_offset = GetPerfSampleDataOffset(*event);
    size_t sample_size = event->header.size - old_offset;

    int size_change = new_len - old_len;
    size_t new_size = event->header.size + size_change;
    size_t new_offset = old_offset + size_change;

    if (size_change > 0) {
      // Allocate memory for a new event.
      event_t* old_event = event;
      malloced_unique_ptr<event_t> new_event(CallocMemoryForEvent(new_size));

      // Copy over everything except filename and sample info.
      memcpy(new_event.get(), old_event, size_of_fixed_event_parts);

      // Copy over the sample info to the correct location.
      char* old_addr = reinterpret_cast<char*>(old_event);
      char* new_addr = reinterpret_cast<char*>(new_event.get());
      memcpy(new_addr + new_offset, old_addr + old_offset, sample_size);

      events_[i] = std::move(new_event);
      event = events_[i].get();
    } else if (size_change < 0) {
      // Move the perf sample data to its new location.
      // Since source and dest could overlap, use memmove instead of memcpy.
      char* start_addr = reinterpret_cast<char*>(event);
      memmove(start_addr + new_offset, start_addr + old_offset, sample_size);
    }

    // Copy over the new filename and fix the size of the event.
    char *event_filename = nullptr;
    if (event->header.type == PERF_RECORD_MMAP) {
      event_filename = event->mmap.filename;
    } else if (event->header.type == PERF_RECORD_MMAP2) {
      event_filename = event->mmap2.filename;
    } else {
      LOG(FATAL) << "Unexpected event type";  // Impossible
    }
    CHECK_GT(snprintf(event_filename, new_filename.size() + 1, "%s",
                      new_filename.c_str()),
             0);
    event->header.size = new_size;
  }

  return true;
}

}  // namespace quipper
