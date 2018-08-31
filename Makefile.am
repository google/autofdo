## Process this file with automake to produce Makefile.in

PROTOBUF = third_party/protobuf
PROTOBUF_DEP = $(PROTOBUF)/src/.libs/libprotobuf.a
PROTOC = ./$(PROTOBUF)/src/protoc

$(PROTOBUF)/configure:
	echo "[AUTOGEN] Preparing protobuf"
	(cd $(PROTOBUF) ; autoreconf -f -i -Wall,no-obsolete)

$(PROTOC) $(PROTOBUF)/src/.libs/libprotobuf.a: $(PROTOBUF)/configure
	echo "[MAKE] Building protobuf"
	(cd third_party/protobuf/; CC="$(CC)" CXX="$(CXX)" LDFLAGS="$(LDFLAGS_$(CONFIG)) -g $(PROTOBUF_LDFLAGS_EXTRA)" CPPFLAGS="$(PIC_CPPFLAGS) $(CPPFLAGS_$(CONFIG)) -g $(PROTOBUF_CPPFLAGS_EXTRA)" ./configure --disable-shared --enable-static $(PROTOBUF_CONFIG_OPTS))
	make -C $(PROTOBUF)

protoc_inputs = \
	third_party/perf_data_converter/src/quipper/perf_data.proto \
	third_party/perf_data_converter/src/quipper/perf_stat.proto

protoc_outputs = \
	third_party/perf_data_converter/src/quipper/perf_data.pb.cc \
	third_party/perf_data_converter/src/quipper/perf_data.pb.h \
	third_party/perf_data_converter/src/quipper/perf_stat.pb.cc \
	third_party/perf_data_converter/src/quipper/perf_stat.pb.h

$(protoc_outputs): $(protoc_inputs) $(PROTOC)
	$(PROTOC) --cpp_out=`dirname $<` -I`dirname $<` $(protoc_inputs)

ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/glog/src -I$(top_srcdir)/third_party/perf_data_converter/src/quipper -I./$(PROTOBUF)/src
AM_CXXFLAGS = -std=gnu++11 -I./$(PROTOBUF)/src
LIBS += -lpthread
LIBELF = -lelf

COMMON_PROFILE_CREATOR_FILES = addr2line.cc gcov.cc instruction_map.cc \
                               module_grouper.cc profile_creator.cc \
                               profile_writer.cc sample_reader.cc \
                               source_info.cc symbol_map.cc profile.cc


bin_PROGRAMS = create_gcov
create_gcov_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) create_gcov.cc
create_gcov_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a $(LIBELF) $(PROTOBUF_DEP)
nodist_create_gcov_SOURCES = $(protoc_outputs)
$(am_create_gcov_OBJECTS): $(protoc_outputs)

bin_PROGRAMS += dump_gcov
dump_gcov_SOURCES = profile_reader.cc symbol_map.cc module_grouper.cc gcov.cc \
                    dump_gcov.cc
dump_gcov_LDADD = libglog.a libgflags.a libsymbolize.a

bin_PROGRAMS += sample_merger
sample_merger_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) sample_merger.cc
sample_merger_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a $(LIBELF) $(PROTOBUF_DEP)
nodist_sample_merger_SOURCES = $(protoc_outputs)
$(am_sample_merger_OBJECTS): $(protoc_outputs)

bin_PROGRAMS += profile_merger
profile_merger_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) profile_reader.cc \
                         profile_merger.cc
profile_merger_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a $(LIBELF) $(PROTOBUF_DEP)
nodist_profile_merger_SOURCES = $(protoc_outputs)
$(am_profile_merger_OBJECTS): $(protoc_outputs)

bin_PROGRAMS += profile_diff
profile_diff_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) profile_reader.cc \
                       profile_diff.cc
profile_diff_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a $(LIBELF) $(PROTOBUF_DEP)
nodist_profile_diff_SOURCES = $(protoc_outputs)
$(am_profile_diff_OBJECTS): $(protoc_outputs)

bin_PROGRAMS += profile_update
profile_update_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) profile_reader.cc \
                         profile_update.cc
profile_update_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a $(LIBELF) $(PROTOBUF_DEP)
nodist_profile_update_SOURCES = $(protoc_outputs)
$(am_profile_update_OBJECTS): $(protoc_outputs)

bin_PROGRAMS += create_llvm_prof
create_llvm_prof_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) \
                           llvm_profile_writer.cc create_llvm_prof.cc
create_llvm_prof_LDADD = $(LLVM_LDFLAGS) $(LLVM_LIBS) libquipper.a libglog.a \
                         libsymbolize.a libgflags.a $(LIBELF) $(PROTOBUF_DEP)
create_llvm_prof_CXXFLAGS = $(LLVM_CXXFLAGS) -DCREATE_LLVM_PROF
nodist_create_llvm_prof_SOURCES = $(protoc_outputs)
$(am_create_llvm_prof_OBJECTS): $(protoc_outputs)

noinst_LIBRARIES = libquipper.a
libquipper_a_SOURCES = \
	third_party/perf_data_converter/src/quipper/address_mapper.cc \
	third_party/perf_data_converter/src/quipper/binary_data_utils.cc \
	third_party/perf_data_converter/src/quipper/buffer_reader.cc \
	third_party/perf_data_converter/src/quipper/buffer_writer.cc \
	third_party/perf_data_converter/src/quipper/data_reader.cc \
	third_party/perf_data_converter/src/quipper/data_writer.cc \
	third_party/perf_data_converter/src/quipper/dso.cc \
	third_party/perf_data_converter/src/quipper/file_reader.cc \
	third_party/perf_data_converter/src/quipper/file_utils.cc \
	third_party/perf_data_converter/src/quipper/perf_data_utils.cc \
	third_party/perf_data_converter/src/quipper/perf_parser.cc \
	third_party/perf_data_converter/src/quipper/perf_reader.cc \
	third_party/perf_data_converter/src/quipper/perf_serializer.cc \
	third_party/perf_data_converter/src/quipper/sample_info_reader.cc \
	third_party/perf_data_converter/src/quipper/huge_page_deducer.cc
nodist_libquipper_a_SOURCES = $(protoc_outputs)
$(am_libquipper_a_OBJECTS): $(protoc_outputs)

noinst_LIBRARIES += libglog.a
libglog_a_SOURCES = glog/src/glog/log_severity.h \
                    glog/src/logging.cc glog/src/raw_logging.cc \
                    glog/src/vlog_is_on.cc \
                    glog/src/utilities.cc src/utilities.h \
                    glog/src/demangle.cc src/demangle.h \
                    glog/src/stacktrace.h \
                    glog/src/stacktrace_generic-inl.h \
                    glog/src/stacktrace_libunwind-inl.h \
                    glog/src/stacktrace_powerpc-inl.h \
                    glog/src/stacktrace_x86-inl.h \
                    glog/src/stacktrace_x86_64-inl.h \
                    glog/src/symbolize.cc src/symbolize.h \
                    glog/src/signalhandler.cc \
                    glog/src/base/mutex.h glog/src/base/googleinit.h \
                    glog/src/base/commandlineflags.h glog/src/googletest.h
libglog_a_CXXFLAGS = -DNO_THREADS

noinst_LIBRARIES += libgflags.a
libgflags_a_SOURCES = gflags/gflags.cc gflags/gflags_completions.cc \
                      gflags/gflags_reporting.cc

noinst_LIBRARIES += libsymbolize.a
libsymbolize_a_SOURCES = symbolize/addr2line_inlinestack.cc \
                         symbolize/bytereader.cc \
                         symbolize/dwarf2reader.cc \
                         symbolize/dwarf3ranges.cc \
                         symbolize/elf_reader.cc \
                         symbolize/functioninfo.cc
