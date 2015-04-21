# Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

BASE_VER ?= 293518
PC_DEPS = openssl protobuf libchrome-$(BASE_VER)
PC_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PC_DEPS))
PC_LIBS := $(shell $(PKG_CONFIG) --libs $(PC_DEPS))

CPPFLAGS += -Iperf $(PC_CFLAGS) -D__STDC_FORMAT_MACROS
LDLIBS += $(PC_LIBS)

TEST_SOURCES = address_mapper_test.cc conversion_utils_test.cc \
	       perf_parser_test.cc perf_reader_test.cc \
	       perf_serializer_test.cc perf_recorder_test.cc utils_test.cc

GENERATED_OBJECTS = perf_data.pb.o
GENERATED_HEADERS = $(GENERATED_OBJECTS:.pb.o=.pb.h)

COMMON_OBJECTS = address_mapper.o conversion_utils.o perf_parser.o \
		 perf_protobuf_io.o \
		 perf_reader.o perf_recorder.o perf_serializer.o utils.o \
		 $(GENERATED_OBJECTS)

COMMON_TEST_OBJECTS = test_utils.o

# Rule for compiling protobuf.
perf_data.pb.h perf_data.pb.cc: perf_data.proto
	protoc --cpp_out=. $^
clean: CLEAN(perf_data.pb.h perf_data.pb.cc)
$(eval $(call add_object_rules,perf_data.pb.o,CXX,cc))

# Dependencies on the protobuf.
quipper.o.depends \
perf_protobuf_io.o.depends \
perf_recorder.o.depends \
perf_serializer.o.depends: perf_data.pb.h

# Rule for compiling main quipper executable.
CXX_BINARY(quipper): quipper.o $(COMMON_OBJECTS)
clean: CLEAN(quipper)
all: CXX_BINARY(quipper)

# Template for unit tests.
define TEST_RULES_template
CXX_BINARY($(1)): LDLIBS += -lgtest -lgmock
CXX_BINARY($(1)): $(1).o $(COMMON_OBJECTS) $(COMMON_TEST_OBJECTS)
clean: CLEAN($(1))
tests: TEST(CXX_BINARY($(1)))
endef

TEST_BINS = $(patsubst $(SRC)/%.cc,%,$(wildcard $(SRC)/*_test.cc))
$(foreach prog,$(TEST_BINS),$(eval $(call TEST_RULES_template,$(prog))))
