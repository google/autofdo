## Process this file with automake to produce Makefile.in

ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/glog/src
AM_CXXFLAGS = -std=gnu++11

COMMON_PROFILE_CREATOR_FILES = addr2line.cc gcov.cc instruction_map.cc \
                               llvm_profile_writer.cc module_grouper.cc \
                               profile_creator.cc profile_writer.cc \
                               sample_reader.cc source_info.cc symbol_map.cc \
                               profile.cc


bin_PROGRAMS = create_gcov
create_gcov_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) create_gcov.cc
create_gcov_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a

bin_PROGRAMS += dump_gcov
dump_gcov_SOURCES = profile_reader.cc symbol_map.cc module_grouper.cc gcov.cc \
                    dump_gcov.cc
dump_gcov_LDADD = libglog.a libgflags.a libsymbolize.a

bin_PROGRAMS += sample_merger
sample_merger_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) sample_merger.cc
sample_merger_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a

bin_PROGRAMS += profile_merger
profile_merger_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) profile_reader.cc \
                         profile_merger.cc
profile_merger_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a

bin_PROGRAMS += profile_diff
profile_diff_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) profile_reader.cc \
                       profile_diff.cc
profile_diff_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a

bin_PROGRAMS += profile_update
profile_update_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) profile_reader.cc \
                         profile_update.cc
profile_update_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a

bin_PROGRAMS += create_llvm_prof
create_llvm_prof_SOURCES = $(COMMON_PROFILE_CREATOR_FILES) create_llvm_prof.cc
create_llvm_prof_LDADD = libquipper.a libglog.a libsymbolize.a libgflags.a

noinst_LIBRARIES = libquipper.a
libquipper_a_SOURCES = quipper/address_mapper.cc quipper/perf_reader.cc \
                       quipper/perf_parser.cc quipper/utils.cc

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
