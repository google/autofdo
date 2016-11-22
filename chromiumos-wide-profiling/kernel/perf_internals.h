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

// The first 64 bits of the perf header, used as a perf data file ID tag.
const uint64_t kPerfMagic = 0x32454c4946524550LL;  // "PERFILE2" little-endian

// These data structures have been copied from the kernel. See files under
// tools/perf/util.

//
// From tools/perf/util/header.h
//

enum {
	HEADER_RESERVED		= 0,	/* always cleared */
	HEADER_FIRST_FEATURE	= 1,
	HEADER_TRACING_DATA	= 1,
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
	HEADER_PMU_MAPPINGS,
	HEADER_GROUP_DESC,
	HEADER_LAST_FEATURE,
	HEADER_FEAT_BITS	= 256,
};

struct perf_file_section {
	u64 offset;
	u64 size;
};

struct perf_file_attr {
	struct perf_event_attr	attr;
	struct perf_file_section	ids;
};

#define BITS_PER_BYTE           8
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))
#define BITS_TO_LONGS(nr)       DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

#define DECLARE_BITMAP(name,bits) \
	unsigned long name[BITS_TO_LONGS(bits)]

struct perf_file_header {
	u64				magic;
	u64				size;
	u64				attr_size;
	struct perf_file_section	attrs;
	struct perf_file_section	data;
	struct perf_file_section	event_types;
	DECLARE_BITMAP(adds_features, HEADER_FEAT_BITS);
};

#undef BITS_PER_BYTE
#undef DIV_ROUND_UP
#undef BITS_TO_LONGS
#undef DECLARE_BITMAP

struct perf_pipe_file_header {
	u64				magic;
	u64				size;
};

//
// From tools/perf/util/event.h
//

struct mmap_event {
	struct perf_event_header header;
	u32 pid, tid;
	u64 start;
	u64 len;
	u64 pgoff;
	char filename[PATH_MAX];
};

struct mmap2_event {
	struct perf_event_header header;
	u32 pid, tid;
	u64 start;
	u64 len;
	u64 pgoff;
	u32 maj;
	u32 min;
	u64 ino;
	u64 ino_generation;
	u32 prot;
	u32 flags;
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

struct lost_samples_event {
	struct perf_event_header header;
	u64 lost;
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

struct throttle_event {
	struct perf_event_header header;
	u64 time;
	u64 id;
	u64 stream_id;
};

#define PERF_SAMPLE_MASK				\
	(PERF_SAMPLE_IP | PERF_SAMPLE_TID |		\
	 PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR |		\
	PERF_SAMPLE_ID | PERF_SAMPLE_STREAM_ID |	\
	 PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD |		\
	 PERF_SAMPLE_IDENTIFIER)

/* perf sample has 16 bits size limit */
#define PERF_SAMPLE_MAX_SIZE (1 << 16)

struct sample_event {
	struct perf_event_header        header;
	u64 array[];
};

#if 0
// PERF_REGS_MAX is arch-dependent, so this is not a useful struct as-is.
struct regs_dump {
	u64 abi;
	u64 mask;
	u64 *regs;

	/* Cached values/mask filled by first register access. */
	u64 cache_regs[PERF_REGS_MAX];
	u64 cache_mask;
};
#endif

struct stack_dump {
	u16 offset;
	u64 size;
	char *data;
};

struct sample_read_value {
	u64 value;
	u64 id;
};

struct sample_read {
	u64 time_enabled;
	u64 time_running;
	union {
		struct {
			u64 nr;
			struct sample_read_value *values;
		} group;
		struct sample_read_value one;
	};
};

struct ip_callchain {
	u64 nr;
	u64 ips[0];
};

struct branch_flags {
	u64 mispred:1;
	u64 predicted:1;
	u64 in_tx:1;
	u64 abort:1;
	u64 cycles:16;
	u64 reserved:44;
};

struct branch_entry {
	u64			from;
	u64			to;
	struct branch_flags	flags;
};

struct branch_stack {
	u64			nr;
	struct branch_entry	entries[0];
};

enum {
	PERF_IP_FLAG_BRANCH		= 1ULL << 0,
	PERF_IP_FLAG_CALL		= 1ULL << 1,
	PERF_IP_FLAG_RETURN		= 1ULL << 2,
	PERF_IP_FLAG_CONDITIONAL	= 1ULL << 3,
	PERF_IP_FLAG_SYSCALLRET		= 1ULL << 4,
	PERF_IP_FLAG_ASYNC		= 1ULL << 5,
	PERF_IP_FLAG_INTERRUPT		= 1ULL << 6,
	PERF_IP_FLAG_TX_ABORT		= 1ULL << 7,
	PERF_IP_FLAG_TRACE_BEGIN	= 1ULL << 8,
	PERF_IP_FLAG_TRACE_END		= 1ULL << 9,
	PERF_IP_FLAG_IN_TX		= 1ULL << 10,
};

#define PERF_IP_FLAG_CHARS "bcrosyiABEx"

#define PERF_BRANCH_MASK		(\
	PERF_IP_FLAG_BRANCH		|\
	PERF_IP_FLAG_CALL		|\
	PERF_IP_FLAG_RETURN		|\
	PERF_IP_FLAG_CONDITIONAL	|\
	PERF_IP_FLAG_SYSCALLRET		|\
	PERF_IP_FLAG_ASYNC		|\
	PERF_IP_FLAG_INTERRUPT		|\
	PERF_IP_FLAG_TX_ABORT		|\
	PERF_IP_FLAG_TRACE_BEGIN	|\
	PERF_IP_FLAG_TRACE_END)

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
	u64 weight;
	u64 transaction;
	u32 cpu;
	u32 raw_size;
	u64 data_src;
	u32 flags;
	u16 insn_len;
	u8  cpumode;
	void *raw_data;
	struct ip_callchain *callchain;
	struct branch_stack *branch_stack;
	//struct regs_dump  user_regs;  // See struct regs_dump above.
	//struct regs_dump  intr_regs;
	struct stack_dump user_stack;
	struct {  // Copied from struct read_event.
		u64 time_enabled;
		u64 time_running;
		u64 id;
	} read;
	// TODO(dhsharp) replace struct above with:
	// struct sample_read read;

	perf_sample() : raw_data(NULL),
			callchain(NULL),
			branch_stack(NULL) {}
	~perf_sample() {
	  delete [] callchain;
	  delete [] branch_stack;
	  delete [] reinterpret_cast<char*>(raw_data);
	}
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

enum perf_user_event_type { /* above any possible kernel type */
	PERF_RECORD_USER_TYPE_START		= 64,
	PERF_RECORD_HEADER_ATTR			= 64,
	PERF_RECORD_HEADER_EVENT_TYPE		= 65, /* depreceated */
	PERF_RECORD_HEADER_TRACING_DATA		= 66,
	PERF_RECORD_HEADER_BUILD_ID		= 67,
	PERF_RECORD_FINISHED_ROUND		= 68,
	/* Google-added pipe-mode event types: */
	PERF_RECORD_HEADER_HOSTNAME		= 69,
	PERF_RECORD_HEADER_OSRELEASE		= 70,
	PERF_RECORD_HEADER_VERSION		= 71,
	PERF_RECORD_HEADER_ARCH			= 72,
	PERF_RECORD_HEADER_NRCPUS		= 73,
	PERF_RECORD_HEADER_CPUDESC		= 74,
	PERF_RECORD_HEADER_CPUID		= 75,
	PERF_RECORD_HEADER_TOTAL_MEM		= 76,
	PERF_RECORD_HEADER_CMDLINE		= 77,
	PERF_RECORD_HEADER_EVENT_DESC		= 78,
	PERF_RECORD_HEADER_CPU_TOPOLOGY		= 79,
	PERF_RECORD_HEADER_NUMA_TOPOLOGY	= 80,
	PERF_RECORD_HEADER_BRANCH_STACK		= 81,
	PERF_RECORD_HEADER_PMU_MAPPINGS		= 82,
	PERF_RECORD_HEADER_GROUP_DESC		= 83,
	PERF_RECORD_HEADER_MAX
};

enum auxtrace_error_type {
	PERF_AUXTRACE_ERROR_ITRACE  = 1,
	PERF_AUXTRACE_ERROR_MAX
};

/*
 * The kernel collects the number of events it couldn't send in a stretch and
 * when possible sends this number in a PERF_RECORD_LOST event. The number of
 * such "chunks" of lost events is stored in .nr_events[PERF_EVENT_LOST] while
 * total_lost tells exactly how many events the kernel in fact lost, i.e. it is
 * the sum of all struct lost_event.lost fields reported.
 *
 * The kernel discards mixed up samples and sends the number in a
 * PERF_RECORD_LOST_SAMPLES event. The number of lost-samples events is stored
 * in .nr_events[PERF_RECORD_LOST_SAMPLES] while total_lost_samples tells
 * exactly how many samples the kernel in fact dropped, i.e. it is the sum of
 * all struct lost_samples_event.lost fields reported.
 *
 * The total_period is needed because by default auto-freq is used, so
 * multipling nr_events[PERF_EVENT_SAMPLE] by a frequency isn't possible to get
 * the total number of low level events, it is necessary to to sum all struct
 * sample_event.period and stash the result in total_period.
 */
struct events_stats {
	u64 total_period;
	u64 total_non_filtered_period;
	u64 total_lost;
	u64 total_lost_samples;
	u64 total_aux_lost;
	u64 total_invalid_chains;
	u32 nr_events[PERF_RECORD_HEADER_MAX];
	u32 nr_non_filtered_samples;
	u32 nr_lost_warned;
	u32 nr_unknown_events;
	u32 nr_invalid_chains;
	u32 nr_unknown_id;
	u32 nr_unprocessable_samples;
	u32 nr_auxtrace_errors[PERF_AUXTRACE_ERROR_MAX];
	u32 nr_proc_map_timeout;
};

enum {
	PERF_CPU_MAP__CPUS = 0,
	PERF_CPU_MAP__MASK = 1,
};

struct cpu_map_entries {
	u16	nr;
	u16	cpu[];
};

struct cpu_map_mask {
	u16	nr;
	u16	long_size;
	unsigned long mask[];
};

struct cpu_map_data {
	u16	type;
	char	data[];
};

struct cpu_map_event {
	struct perf_event_header	header;
	struct cpu_map_data		data;
};

struct attr_event {
	struct perf_event_header header;
	struct perf_event_attr attr;
	u64 id[];
};

enum {
	PERF_EVENT_UPDATE__UNIT  = 0,
	PERF_EVENT_UPDATE__SCALE = 1,
	PERF_EVENT_UPDATE__NAME  = 2,
	PERF_EVENT_UPDATE__CPUS  = 3,
};

struct event_update_event_cpus {
	struct cpu_map_data cpus;
};

struct event_update_event_scale {
	double scale;
};

struct event_update_event {
	struct perf_event_header header;
	u64 type;
	u64 id;

	char data[];
};

#define MAX_EVENT_NAME 64

struct perf_trace_event_type {
	u64	event_id;
	char	name[MAX_EVENT_NAME];
};

#undef MAX_EVENT_NAME

struct event_type_event {
	struct perf_event_header header;
	struct perf_trace_event_type event_type;
};

struct tracing_data_event {
	struct perf_event_header header;
	u32 size;
};

struct id_index_entry {
	u64 id;
	u64 idx;
	u64 cpu;
	u64 tid;
};

struct id_index_event {
	struct perf_event_header header;
	u64 nr;
	struct id_index_entry entries[0];
};

struct auxtrace_info_event {
	struct perf_event_header header;
	u32 type;
	u32 reserved__; /* For alignment */
	u64 priv[];
};

struct auxtrace_event {
	struct perf_event_header header;
	u64 size;
	u64 offset;
	u64 reference;
	u32 idx;
	u32 tid;
	u32 cpu;
	u32 reserved__; /* For alignment */
};

#define MAX_AUXTRACE_ERROR_MSG 64

struct auxtrace_error_event {
	struct perf_event_header header;
	u32 type;
	u32 code;
	u32 cpu;
	u32 pid;
	u32 tid;
	u32 reserved__; /* For alignment */
	u64 ip;
	char msg[MAX_AUXTRACE_ERROR_MSG];
};

struct aux_event {
	struct perf_event_header header;
	u64	aux_offset;
	u64	aux_size;
	u64	flags;
};

struct itrace_start_event {
	struct perf_event_header header;
	u32 pid, tid;
};

struct context_switch_event {
	struct perf_event_header header;
	u32 next_prev_pid;
	u32 next_prev_tid;
};

struct thread_map_event_entry {
	u64	pid;
	char	comm[16];
};

struct thread_map_event {
	struct perf_event_header	header;
	u64				nr;
	struct thread_map_event_entry	entries[];
};

enum {
	PERF_STAT_CONFIG_TERM__AGGR_MODE	= 0,
	PERF_STAT_CONFIG_TERM__INTERVAL		= 1,
	PERF_STAT_CONFIG_TERM__SCALE		= 2,
	PERF_STAT_CONFIG_TERM__MAX		= 3,
};

struct stat_config_event_entry {
	u64	tag;
	u64	val;
};

struct stat_config_event {
	struct perf_event_header	header;
	u64				nr;
	struct stat_config_event_entry	data[];
};

struct stat_event {
	struct perf_event_header	header;

	u64	id;
	u32	cpu;
	u32	thread;

	union {
		struct {
			u64 val;
			u64 ena;
			u64 run;
		};
		u64 values[3];
	};
};

enum {
	PERF_STAT_ROUND_TYPE__INTERVAL	= 0,
	PERF_STAT_ROUND_TYPE__FINAL	= 1,
};

struct stat_round_event {
	struct perf_event_header	header;
	u64				type;
	u64				time;
};

struct time_conv_event {
	struct perf_event_header header;
	u64 time_shift;
	u64 time_mult;
	u64 time_zero;
};

union perf_event {
	struct perf_event_header	header;
	struct mmap_event		mmap;
	struct mmap2_event		mmap2;
	struct comm_event		comm;
	struct fork_event		fork;
	struct lost_event		lost;
	struct lost_samples_event	lost_samples;
	struct read_event		read;
	struct throttle_event		throttle;
	struct sample_event		sample;
	struct attr_event		attr;
	struct event_update_event	event_update;
	struct event_type_event		event_type;
	struct tracing_data_event	tracing_data;
	struct build_id_event		build_id;
	struct id_index_event		id_index;
	struct auxtrace_info_event	auxtrace_info;
	struct auxtrace_event		auxtrace;
	struct auxtrace_error_event	auxtrace_error;
	struct aux_event		aux;
	struct itrace_start_event	itrace_start;
	struct context_switch_event	context_switch;
	struct thread_map_event		thread_map;
	struct cpu_map_event		cpu_map;
	struct stat_config_event	stat_config;
	struct stat_event		stat;
	struct stat_round_event		stat_round;
	struct time_conv_event		time_conv;
};

typedef perf_event event_t;

}  // namespace quipper

#endif /*PERF_INTERNALS_H_*/
