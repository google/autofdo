// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_PERF_SERIALIZER_H_
#define CHROMIUMOS_WIDE_PROFILING_PERF_SERIALIZER_H_

#include <string>
#include <vector>

#include "base/macros.h"

#include "perf_parser.h"
#include "quipper_proto.h"

namespace quipper {

class PerfSerializer;

// Functor to serialize a vector of perf structs to a Proto repeated field,
// using a method serializes one such item.
// Overriding RefT allows serializing a vector of pointers to struct with
// a serialize member fuction that takes const T* (instead of T*const&).
template <typename Proto, typename T, typename RefT = const T&>
struct VectorSerializer {
  bool operator()(const std::vector<T>& from,
                  RepeatedPtrField<Proto>* to) const {
    to->Reserve(from.size());
    for (size_t i = 0; i != from.size(); ++i) {
      Proto* to_element = to->Add();
      if (to_element == NULL) {
        return false;
      }
      if (!(p->*serialize)(from[i], to_element)) {
        return false;
      }
    }
    return true;
  }
  const PerfSerializer* p;
  bool (PerfSerializer::*serialize)(RefT, Proto*) const;
};

// Functor to deserialize a Proto repeated field to a vector of perf structs,
// using a method that deserializes one Proto.
template <typename Proto, typename T>
struct VectorDeserializer {
  bool operator()(const RepeatedPtrField<Proto>& from,
                  std::vector<T>* to) const {
    to->resize(from.size());
    for (int i = 0; i != from.size(); ++i) {
      if (!(p->*deserialize)(from.Get(i), &(*to)[i])) {
        return false;
      }
    }
    return true;
  }
  const PerfSerializer* p;
  bool (PerfSerializer::*deserialize)(const Proto&, T*) const;
};

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

  void set_serialize_sorted_events(bool sorted) {
    serialize_sorted_events_ = sorted;
  }

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
  bool SerializeEventPointer(
      const ParsedEvent* event,
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
      const RepeatedPtrField<PerfDataProto_PerfBuildID>& from,
      std::vector<build_id_event*>* to) const;

  bool SerializeMetadata(PerfDataProto* to) const;
  bool DeserializeMetadata(const PerfDataProto& from);

  bool SerializeBuildIDEvent(const build_id_event* from,
                             PerfDataProto_PerfBuildID* to) const;
  bool DeserializeBuildIDEvent(const PerfDataProto_PerfBuildID& from,
                               build_id_event** to) const;

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


  const VectorSerializer<PerfDataProto_PerfFileAttr, PerfFileAttr>
      SerializePerfFileAttrs = {this, &PerfSerializer::SerializePerfFileAttr};
  const VectorDeserializer<PerfDataProto_PerfFileAttr, PerfFileAttr>
      DeserializePerfFileAttrs = {
        this, &PerfSerializer::DeserializePerfFileAttr};

  const VectorSerializer<PerfDataProto_PerfEventType, perf_trace_event_type>
      SerializePerfEventTypes = {this, &PerfSerializer::SerializePerfEventType};
  const VectorDeserializer<PerfDataProto_PerfEventType, perf_trace_event_type>
      DeserializePerfEventTypes = {
        this, &PerfSerializer::DeserializePerfEventType};

  const VectorSerializer<PerfDataProto_PerfEvent, ParsedEvent>
      SerializeEvents = {this, &PerfSerializer::SerializeEvent};
  const VectorSerializer<PerfDataProto_PerfEvent,
                         ParsedEvent*, const ParsedEvent*>
      SerializeEventPointers = {this, &PerfSerializer::SerializeEventPointer};
  const VectorDeserializer<PerfDataProto_PerfEvent, ParsedEvent>
      DeserializeEvents = {this, &PerfSerializer::DeserializeEvent};

  const VectorSerializer<PerfDataProto_PerfBuildID,
                         build_id_event*, const build_id_event*>
      SerializeBuildIDEvents = {this, &PerfSerializer::SerializeBuildIDEvent};
  const VectorDeserializer<PerfDataProto_PerfBuildID, build_id_event*>
      DeserializeBuildIDEvents = {
        this, &PerfSerializer::DeserializeBuildIDEvent};

  const VectorSerializer<PerfDataProto_PerfUint32Metadata, PerfUint32Metadata>
      SerializeUint32Metadata = {
        this, &PerfSerializer::SerializeSingleUint32Metadata};
  const VectorDeserializer<PerfDataProto_PerfUint32Metadata, PerfUint32Metadata>
      DeserializeUint32Metadata = {
        this, &PerfSerializer::DeserializeSingleUint32Metadata};

  const VectorSerializer<PerfDataProto_PerfUint64Metadata, PerfUint64Metadata>
      SerializeUint64Metadata = {
        this, &PerfSerializer::SerializeSingleUint64Metadata};
  const VectorDeserializer<PerfDataProto_PerfUint64Metadata, PerfUint64Metadata>
      DeserializeUint64Metadata = {
        this, &PerfSerializer::DeserializeSingleUint64Metadata};

  const VectorSerializer<PerfDataProto_PerfNodeTopologyMetadata,
                         PerfNodeTopologyMetadata>
      SerializeNUMATopologyMetadata = {
        this, &PerfSerializer::SerializeNodeTopologyMetadata};
  const VectorDeserializer<PerfDataProto_PerfNodeTopologyMetadata,
                           PerfNodeTopologyMetadata>
      DeserializeNUMATopologyMetadata = {
        this, &PerfSerializer::DeserializeNodeTopologyMetadata};

  // Set this flag to serialize perf events in chronological order, rather than
  // the order in which they appear in the raw data.
  bool serialize_sorted_events_;

  DISALLOW_COPY_AND_ASSIGN(PerfSerializer);
};

}  // namespace quipper

#endif  // CHROMIUMOS_WIDE_PROFILING_PERF_SERIALIZER_H_
