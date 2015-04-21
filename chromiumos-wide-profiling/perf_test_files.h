// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMIUMOS_WIDE_PROFILING_PERF_TEST_FILES_H_
#define CHROMIUMOS_WIDE_PROFILING_PERF_TEST_FILES_H_

// TODO(sque): Re-enable callgraph testing when longer-term
// changes to quipper are done.
// #define TEST_CALLGRAPH

namespace perf_test_files {

// The following perf data contains the following event types, as passed to
// perf record via the -e option:
// - cycles
// - instructions
// - cache-references
// - cache-misses
// - branches
// - branch-misses

const char* kPerfDataFiles[] = {
  // Obtained with "perf record -- echo > /dev/null"
  "perf.data.singleprocess",

  // Obtained with "perf record -a -- sleep $N", for N in {0, 1, 5}.
  "perf.data.systemwide.0",
  "perf.data.systemwide.1",
  "perf.data.systemwide.5",

  // Obtained with "perf record -a -- sleep $N", for N in {0, 1, 5}.
  // While in the background, this loop is running:
  //   while true; do ls > /dev/null; done
  "perf.data.busy.0",
  "perf.data.busy.1",
  "perf.data.busy.5",

  // Obtained with "perf record -a -- sleep 2"
  // While in the background, this loop is running:
  //   while true; do restart powerd; sleep .2; done
  "perf.data.forkexit",

#ifdef TEST_CALLGRAPH
  // Obtained with "perf record -a -g -- sleep 2"
  "perf.data.callgraph",
#endif
  // Obtained with "perf record -a -b -- sleep 2"
  "perf.data.branch",
#ifdef TEST_CALLGRAPH
  // Obtained with "perf record -a -g -b -- sleep 2"
  "perf.data.callgraph_and_branch",
#endif

  // Obtained with "perf record -a -R -- sleep 2"
  "perf.data.raw",
#ifdef TEST_CALLGRAPH
  // Obtained with "perf record -a -R -g -b -- sleep 2"
  "perf.data.raw_callgraph_branch",
#endif

  // Data from other architectures.
  "perf.data.i686",     // 32-bit x86
  "perf.data.armv7",    // ARM v7
  "perf.data.armv7.perf_3.14",      // ARM v7 obtained using perf 3.14.

  // Same as above, obtained from a system running kernel-next.
  "perf.data.singleprocess.next",
  "perf.data.systemwide.0.next",
  "perf.data.systemwide.1.next",
  "perf.data.systemwide.5.next",
  "perf.data.busy.0.next",
  "perf.data.busy.1.next",
  "perf.data.busy.5.next",
  "perf.data.forkexit.next",
#ifdef TEST_CALLGRAPH
  "perf.data.callgraph.next",
#endif
  "perf.data.branch.next",
#ifdef TEST_CALLGRAPH
  "perf.data.callgraph_and_branch.next",
#endif

  // Obtained from a system that uses NUMA topology.
  "perf.data.numatopology",

  // Perf data that contains hardware and software events.
  // Command:
  //    perf record -a -c 1000000 -e cycles,branch-misses,cpu-clock -- sleep 2
  // HW events are cycles and branch-misses, SW event is cpu-clock.
  // This also tests non-consecutive event types.
  "perf.data.hw_and_sw",

  // This test first mmap()s a DSO, then fork()s to copy the mapping to the
  // child and then modifies the mapping by mmap()ing a DSO on top of the old
  // one. It then records SAMPLEs events in the child. It ensures the SAMPLEs in
  // the child are attributed to the first DSO that was mmap()ed, not the second
  // one.
  "perf.data.remmap",

  // This is sample with a frequency higher than the max frequency, so it has
  // throttle and unthrottle events.
  "perf.data.throttle.next",
};

const char* kPerfPipedDataFiles[] = {
  "perf.data.piped.host",
  "perf.data.piped.target",
  "perf.data.piped.target.throttled",
  // From system running kernel-next.
  "perf.data.piped.target.next",

  // Piped data that contains hardware and software events.
  // Command:
  //    perf record -a -c 1000000 -e cycles,branch-misses,cpu-clock -o -
  //        -- sleep 2
  // HW events are cycles and branch-misses, SW event is cpu-clock.
  "perf.data.piped.hw_and_sw",

  // Piped data with extra data at end.
  "perf.data.piped.extrabyte",
  "perf.data.piped.extradata",
};

const char* kCorruptedPerfPipedDataFiles[] = {
  // Has a SAMPLE event with size set to zero. Don't go into an infinite loop!
  "perf.data.piped.corrupted.zero_size_sample",
};

const char* kPerfDataProtoFiles[] = {
  "perf.callgraph.pb_text",
};

}  // namespace perf_test_files

#endif  // CHROMIUMOS_WIDE_PROFILING_PERF_TEST_FILES_H_
