// Copied from kernel sources. See COPYING for license details.

#ifndef PERF_INTERNALS_H_
#define PERF_INTERNALS_H_

#include <linux/limits.h>
#include <stdint.h>

#include "perf_event.h"

namespace quipper {

// These typedefs are from tools/perf/util/types.h in the kernel.
typedef uint64_t           u64;
typedef int64_t            s64;
typedef unsigned int	   u32;
typedef signed int	   s32;
typedef unsigned short	   u16;
typedef signed short	   s16;
typedef unsigned char	   u8;
typedef signed char	   s8;

#define BITS_PER_BYTE           8
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))
#define BITS_TO_LONGS(nr)       DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

#define DECLARE_BITMAP(name,bits) \
	unsigned long name[BITS_TO_LONGS(bits)]

#define MAX_EVENT_NAME 64

// These data structures have been copied from the kernel. See files under
// tools/perf/util.

enum {
	HEADER_RESERVED		= 0,	/* always cleared */
	HEADER_FIRST_FEATURE	= 1,
	HEADER_TRACE_INFO	= 1,
	HEADER_BUILD_ID,

	HEADER_HOSTNAME,
	HEADER_OSRELEASE,
	HEADER_VERSION,
	HEADER_ARCH,
	HEADER_NRCPUS,
	HEADER_CPUDESC,
	HEADER_CPUID,
	HEADER_TOTAL_MEM,
	HEADER_CMDLINE,
	HEADER_EVENT_DESC,
	HEADER_CPU_TOPOLOGY,
	HEADER_NUMA_TOPOLOGY,
	HEADER_BRANCH_STACK,
	HEADER_LAST_FEATURE,
	HEADER_FEAT_BITS	= 256,
};

/* pseudo samples injected by perf-inject */
enum perf_user_event_type { /* above any possible kernel type */
        PERF_RECORD_USER_TYPE_START             = 64,
        PERF_RECORD_HEADER_ATTR                 = 64,
        PERF_RECORD_HEADER_EVENT_TYPE           = 65,
        PERF_RECORD_HEADER_TRACING_DATA         = 66,
        PERF_RECORD_HEADER_BUILD_ID             = 67,
        PERF_RECORD_FINISHED_ROUND              = 68,
        PERF_RECORD_HEADER_HOSTNAME             = 69,
        PERF_RECORD_HEADER_OSRELEASE            = 70,
        PERF_RECORD_HEADER_VERSION              = 71,
        PERF_RECORD_HEADER_ARCH                 = 72,
        PERF_RECORD_HEADER_NRCPUS               = 73,
        PERF_RECORD_HEADER_CPUDESC              = 74,
        PERF_RECORD_HEADER_CPUID                = 75,
        PERF_RECORD_HEADER_TOTAL_MEM            = 76,
        PERF_RECORD_HEADER_CMDLINE              = 77,
        PERF_RECORD_HEADER_EVENT_DESC           = 78,
        PERF_RECORD_HEADER_CPU_TOPOLOGY         = 79,
        PERF_RECORD_HEADER_NUMA_TOPOLOGY        = 80,
        PERF_RECORD_HEADER_PMU_MAPPINGS         = 81,
        PERF_RECORD_HEADER_MAX
};

struct perf_file_section {
	u64 offset;
	u64 size;
};

struct perf_file_attr {
	struct perf_event_attr	attr;
	struct perf_file_section	ids;
};

struct perf_trace_event_type {
  u64     event_id;
  char    name[MAX_EVENT_NAME];
};

struct perf_file_header {
	u64				magic;
	u64				size;
	u64				attr_size;
	struct perf_file_section	attrs;
	struct perf_file_section	data;
	struct perf_file_section	event_types;
	DECLARE_BITMAP(adds_features, HEADER_FEAT_BITS);
};

struct perf_pipe_file_header {
	u64				magic;
	u64				size;
};

struct attr_event {
	struct perf_event_header header;
	struct perf_event_attr attr;
	uint64_t id[];
};

struct event_type_event {
	struct perf_event_header header;
	struct perf_trace_event_type event_type;
};

struct event_desc_event {
  struct perf_event_header header;
  uint32_t num_events;
  uint32_t event_header_size;
  uint8_t more_data[];
};

enum {
	SHOW_KERNEL	= 1,
	SHOW_USER	= 2,
	SHOW_HV		= 4,
};

/*
 * PERF_SAMPLE_IP | PERF_SAMPLE_TID | *
 */
struct ip_event {
	struct perf_event_header header;
	u64 ip;
	u32 pid, tid;
	unsigned char __more_data[];
};

struct mmap_event {
	struct perf_event_header header;
	u32 pid, tid;
	u64 start;
	u64 len;
	u64 pgoff;
	char filename[PATH_MAX];
};

struct comm_event {
	struct perf_event_header header;
	u32 pid, tid;
	char comm[16];
};

struct fork_event {
	struct perf_event_header header;
	u32 pid, ppid;
	u32 tid, ptid;
	u64 time;
};

struct lost_event {
	struct perf_event_header header;
	u64 id;
	u64 lost;
};

// This struct is found in comments in perf_event.h, and can be found as a
// struct in tools/perf/util/python.c in the kernel.
struct throttle_event {
        struct perf_event_header header;
        u64                      time;
        u64                      id;
        u64                      stream_id;
};

/*
 * PERF_FORMAT_ENABLED | PERF_FORMAT_RUNNING | PERF_FORMAT_ID
 */
struct read_event {
	struct perf_event_header header;
	u32 pid, tid;
	u64 value;
	u64 time_enabled;
	u64 time_running;
	u64 id;
};

struct sample_event{
	struct perf_event_header        header;
	u64 array[];
};

// Taken from tools/perf/util/include/linux/kernel.h
#define ALIGN(x,a)		__ALIGN_MASK(x,(typeof(x))(a)-1)
#define __ALIGN_MASK(x,mask)	(((x)+(mask))&~(mask))

// If this is changed, kBuildIDArraySize in perf_reader.h must also be changed.
#define BUILD_ID_SIZE 20

struct build_id_event {
	struct perf_event_header header;
	pid_t			 pid;
	u8			 build_id[ALIGN(BUILD_ID_SIZE, sizeof(u64))];
	char			 filename[];
};

#undef ALIGN
#undef __ALIGN_MASK
#undef BUILD_ID_SIZE

// The addition of throttle_event is a custom addition for quipper.
// It is used for both THROTTLE and UNTHROTTLE events.
typedef union event_union {
	struct perf_event_header	header;
	struct ip_event			ip;
	struct mmap_event		mmap;
	struct comm_event		comm;
	struct fork_event		fork;
	struct lost_event		lost;
	struct throttle_event		throttle;
	struct read_event		read;
	struct sample_event		sample;
	struct build_id_event		build_id;
} event_t;

struct ip_callchain {
	u64 nr;
	u64 ips[0];
};

struct branch_flags {
	u64 mispred:1;
	u64 predicted:1;
	u64 reserved:62;
};

struct branch_entry {
	u64				from;
	u64				to;
	struct branch_flags flags;
};

struct branch_stack {
	u64				nr;
	struct branch_entry	entries[0];
};

// All the possible fields of a perf sample.  This is not an actual data
// structure found in raw perf data, as each field may or may not be present in
// the data.
struct perf_sample {
	u64 ip;
	u32 pid, tid;
	u64 time;
	u64 addr;
	u64 id;
	u64 stream_id;
	u64 period;
	u32 cpu;
	struct {  // Copied from struct read_event.
		u64 time_enabled;
		u64 time_running;
		u64 id;
	} read;
	u32 raw_size;
	void *raw_data;
	struct ip_callchain *callchain;
	struct branch_stack *branch_stack;

	perf_sample() : raw_data(NULL),
			callchain(NULL),
			branch_stack(NULL) {}
	~perf_sample() {
	  if (callchain) {
	    delete [] callchain;
	    callchain = NULL;
	  }
	  if (branch_stack) {
	    delete [] branch_stack;
	    branch_stack = NULL;
	  }
	  if (raw_data) {
	    delete [] reinterpret_cast<char*>(raw_data);
	    raw_data = NULL;
	  }
	}
};

// End data structures copied from the kernel.

#undef BITS_PER_BYTE
#undef DIV_ROUND_UP
#undef BITS_TO_LONGS
#undef DECLARE_BITMAP
#undef MAX_EVENT_NAME

}  // namespace quipper

#endif /*PERF_INTERNALS_H_*/
