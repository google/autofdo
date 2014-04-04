// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERF_SERIALIZER_H_
#define PERF_SERIALIZER_H_

#include <string>
#include <vector>

#include "base/basictypes.h"

#include "perf_parser.h"
#include "quipper_proto.h"

namespace quipper {

class PerfSerializer : public PerfParser {
 public:
  PerfSerializer();
  ~PerfSerializer();

  // Converts raw perf file to protobuf.
  bool SerializeFromFile(const string& filename,
                         quipper::PerfDataProto* perf_data_proto);

  // Converts data inside PerfSerializer to protobuf.
  bool Serialize(quipper::PerfDataProto* perf_data_proto);

  // Converts perf data protobuf to perf data file.
  bool DeserializeToFile(const quipper::PerfDataProto& perf_data_proto,
                         const string& filename);

  // Reads in contents of protobuf to store locally.  Does not write to any
  // output files.
  bool Deserialize(const quipper::PerfDataProto& perf_data_proto);

 private:
  bool SerializePerfFileAttr(
      const PerfFileAttr& perf_file_attr,
      quipper::PerfDataProto_PerfFileAttr* perf_file_attr_proto) const;
  bool DeserializePerfFileAttr(
      const quipper::PerfDataProto_PerfFileAttr& perf_file_attr_proto,
      PerfFileAttr* perf_file_attr) const;

  bool SerializePerfEventAttr(
      const perf_event_attr& perf_event_attr,
      quipper::PerfDataProto_PerfEventAttr* perf_event_attr_proto) const;
  bool DeserializePerfEventAttr(
      const quipper::PerfDataProto_PerfEventAttr& perf_event_attr_proto,
      perf_event_attr* perf_event_attr) const;

  bool SerializePerfEventType(
      const perf_trace_event_type& event_type,
      quipper::PerfDataProto_PerfEventType* event_type_proto) const;
  bool DeserializePerfEventType(
      const quipper::PerfDataProto_PerfEventType& event_type_proto,
      perf_trace_event_type* event_type) const;

  bool SerializeEvent(const ParsedEvent& event,
                      quipper::PerfDataProto_PerfEvent* event_proto) const;
  bool DeserializeEvent(
      const quipper::PerfDataProto_PerfEvent& event_proto,
      ParsedEvent* event) const;

  bool SerializeEventHeader(
      const perf_event_header& header,
      quipper::PerfDataProto_EventHeader* header_proto) const;
  bool DeserializeEventHeader(
      const quipper::PerfDataProto_EventHeader& header_proto,
      perf_event_header* header) const;

  bool SerializeRecordSample(const event_t& event,
                             quipper::PerfDataProto_SampleEvent* sample) const;
  bool DeserializeRecordSample(
      const quipper::PerfDataProto_SampleEvent& sample,
      event_t* event) const;

  bool SerializeMMapSample(const event_t& event,
                           quipper::PerfDataProto_MMapEvent* sample) const;
  bool DeserializeMMapSample(
      const quipper::PerfDataProto_MMapEvent& sample,
      event_t* event) const;

  bool SerializeCommSample(
      const event_t& event,
      quipper::PerfDataProto_CommEvent* sample) const;
  bool DeserializeCommSample(
      const quipper::PerfDataProto_CommEvent& sample,
      event_t* event) const;

  bool SerializeForkSample(const event_t& event,
                           quipper::PerfDataProto_ForkEvent* sample) const;
  bool DeserializeForkSample(
      const quipper::PerfDataProto_ForkEvent& sample,
      event_t* event) const;

  bool SerializeLostSample(const event_t& event,
                           quipper::PerfDataProto_LostEvent* sample) const;
  bool DeserializeLostSample(
      const quipper::PerfDataProto_LostEvent& sample,
      event_t* event) const;

  bool SerializeThrottleSample(const event_t& event,
                           quipper::PerfDataProto_ThrottleEvent* sample) const;
  bool DeserializeThrottleSample(
      const quipper::PerfDataProto_ThrottleEvent& sample,
      event_t* event) const;

  bool SerializeReadSample(const event_t& event,
                           quipper::PerfDataProto_ReadEvent* sample) const;
  bool DeserializeReadSample(
      const quipper::PerfDataProto_ReadEvent& sample,
      event_t* event) const;

  bool SerializeSampleInfo(
      const event_t& event,
      quipper::PerfDataProto_SampleInfo* sample_info) const;
  bool DeserializeSampleInfo(
      const quipper::PerfDataProto_SampleInfo& info,
      event_t* event) const;

  bool SerializeBuildIDs(
      const std::vector<build_id_event*>& from,
      RepeatedPtrField<PerfDataProto_PerfBuildID>* to)
      const;
  bool DeserializeBuildIDs(
      const
      RepeatedPtrField<PerfDataProto_PerfBuildID>& from,
      std::vector<build_id_event*>* to) const;

  bool SerializeBuildIDEvent(build_id_event* const& from,
                             PerfDataProto_PerfBuildID* to) const;
  bool DeserializeBuildIDEvent(const PerfDataProto_PerfBuildID& from,
                               build_id_event** to) const;

  bool SerializeSingleStringMetadata(
      const PerfStringMetadata& metadata,
      PerfDataProto_PerfStringMetadata* proto_metadata) const;
  bool DeserializeSingleStringMetadata(
      const PerfDataProto_PerfStringMetadata& proto_metadata,
      PerfStringMetadata* metadata) const;

  bool SerializeSingleUint32Metadata(
      const PerfUint32Metadata& metadata,
      PerfDataProto_PerfUint32Metadata* proto_metadata) const;
  bool DeserializeSingleUint32Metadata(
      const PerfDataProto_PerfUint32Metadata& proto_metadata,
      PerfUint32Metadata* metadata) const;

  bool SerializeSingleUint64Metadata(
      const PerfUint64Metadata& metadata,
      PerfDataProto_PerfUint64Metadata* proto_metadata) const;
  bool DeserializeSingleUint64Metadata(
      const PerfDataProto_PerfUint64Metadata& proto_metadata,
      PerfUint64Metadata* metadata) const;

  bool SerializeCPUTopologyMetadata(
      const PerfCPUTopologyMetadata& metadata,
      PerfDataProto_PerfCPUTopologyMetadata* proto_metadata) const;
  bool DeserializeCPUTopologyMetadata(
      const PerfDataProto_PerfCPUTopologyMetadata& proto_metadata,
      PerfCPUTopologyMetadata* metadata) const;

  bool SerializeNodeTopologyMetadata(
      const PerfNodeTopologyMetadata& metadata,
      PerfDataProto_PerfNodeTopologyMetadata* proto_metadata) const;
  bool DeserializeNodeTopologyMetadata(
      const PerfDataProto_PerfNodeTopologyMetadata& proto_metadata,
      PerfNodeTopologyMetadata* metadata) const;

  // Populates |parsed_events_| with pointers event_t and perf_sample structs in
  // each corresponding |events_| struct.
  void SetRawEvents(size_t num_events);

#define SERIALIZEVECTORFUNCTION(name, vec_type, proto_type, function) \
bool name(const std::vector<vec_type>& from, \
          RepeatedPtrField<proto_type>* to) const { \
  to->Reserve(from.size()); \
  for (size_t i = 0; i < from.size(); i++) { \
    proto_type* to_element = to->Add(); \
    if (to_element == NULL) \
      return false; \
    if (!function(from.at(i), to->Mutable(i))) \
      return false; \
  } \
  return true; \
}

#define DESERIALIZEVECTORFUNCTION(name, vec_type, proto_type, function) \
bool name(const RepeatedPtrField<proto_type>& from, \
          std::vector<vec_type>* to) const { \
  to->resize(from.size()); \
  for (int i = 0; i < from.size(); i++) { \
    if (!function(from.Get(i), &to->at(i))) \
      return false; \
  } \
  return true; \
}

  SERIALIZEVECTORFUNCTION(SerializePerfFileAttrs, PerfFileAttr,
                          quipper::PerfDataProto_PerfFileAttr,
                          SerializePerfFileAttr)
  DESERIALIZEVECTORFUNCTION(DeserializePerfFileAttrs, PerfFileAttr,
                            quipper::PerfDataProto_PerfFileAttr,
                            DeserializePerfFileAttr)

  SERIALIZEVECTORFUNCTION(SerializePerfEventTypes, perf_trace_event_type,
                          quipper::PerfDataProto_PerfEventType,
                          SerializePerfEventType)
  DESERIALIZEVECTORFUNCTION(DeserializePerfEventTypes, perf_trace_event_type,
                            quipper::PerfDataProto_PerfEventType,
                            DeserializePerfEventType)

  SERIALIZEVECTORFUNCTION(SerializeEvents, ParsedEvent,
                          quipper::PerfDataProto_PerfEvent,
                          SerializeEvent)
  DESERIALIZEVECTORFUNCTION(DeserializeEvents, ParsedEvent,
                            quipper::PerfDataProto_PerfEvent,
                            DeserializeEvent)

  SERIALIZEVECTORFUNCTION(SerializeBuildIDEvents, build_id_event*,
                          quipper::PerfDataProto_PerfBuildID,
                          SerializeBuildIDEvent)
  DESERIALIZEVECTORFUNCTION(DeserializeBuildIDEvents, build_id_event*,
                            quipper::PerfDataProto_PerfBuildID,
                            DeserializeBuildIDEvent)

  SERIALIZEVECTORFUNCTION(SerializeStringMetadata, PerfStringMetadata,
                          quipper::PerfDataProto_PerfStringMetadata,
                          SerializeSingleStringMetadata)
  DESERIALIZEVECTORFUNCTION(DeserializeStringMetadata, PerfStringMetadata,
                            quipper::PerfDataProto_PerfStringMetadata,
                            DeserializeSingleStringMetadata)

  SERIALIZEVECTORFUNCTION(SerializeUint32Metadata, PerfUint32Metadata,
                          quipper::PerfDataProto_PerfUint32Metadata,
                          SerializeSingleUint32Metadata)
  DESERIALIZEVECTORFUNCTION(DeserializeUint32Metadata, PerfUint32Metadata,
                            quipper::PerfDataProto_PerfUint32Metadata,
                            DeserializeSingleUint32Metadata)

  SERIALIZEVECTORFUNCTION(SerializeUint64Metadata, PerfUint64Metadata,
                          quipper::PerfDataProto_PerfUint64Metadata,
                          SerializeSingleUint64Metadata)
  DESERIALIZEVECTORFUNCTION(DeserializeUint64Metadata, PerfUint64Metadata,
                            quipper::PerfDataProto_PerfUint64Metadata,
                            DeserializeSingleUint64Metadata)

  SERIALIZEVECTORFUNCTION(SerializeNUMATopologyMetadata,
                          PerfNodeTopologyMetadata,
                          quipper::PerfDataProto_PerfNodeTopologyMetadata,
                          SerializeNodeTopologyMetadata)
  DESERIALIZEVECTORFUNCTION(DeserializeNUMATopologyMetadata,
                            PerfNodeTopologyMetadata,
                            quipper::PerfDataProto_PerfNodeTopologyMetadata,
                            DeserializeNodeTopologyMetadata)

  DISALLOW_COPY_AND_ASSIGN(PerfSerializer);
};

}  // namespace quipper

#endif  // PERF_SERIALIZER_H_
