#include "perfdata_reader.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binary_address_branch.h"
#include "branch_frequencies.h"
#include "lbr_aggregation.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_perf_data_provider.h"
#include "spe_tid_pid_provider.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "third_party/abseil/absl/strings/match.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/str_join.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "third_party/abseil/absl/types/span.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include "quipper/arm_spe_decoder.h"
#include "quipper/perf_parser.h"
#include "quipper/perf_reader.h"
#include "base/logging.h"
#include "base/status_macros.h"

namespace {
// Convert binary data stored in data[...] into text representation.
std::string BinaryDataToAscii(absl::string_view data) {
  std::string ascii(data.size() * 2, 0);
  const char heximal[] = "0123456789abcdef";
  for (int i = 0; i < data.size(); ++i) {
    uint8_t d(data[i]);
    ascii[i * 2] = heximal[((d >> 4) & 0xf)];
    ascii[i * 2 + 1] = heximal[(d & 0xf)];
  }
  return ascii;
}
}  // namespace

namespace devtools_crosstool_autofdo {

// Given "n", compare it to each of mmap_event.filename. If "n" is absolute,
// then we compare "n" w/ mmap_event.filename. Otherwise we compare n's name
// part w/ mmap_event.filename's name part.
struct MMapSelector {
  explicit MMapSelector(const absl::flat_hash_set<std::string> &name_set) {
    std::string n = *(name_set.begin());
    // Similar to handling kernel image name -> mmap name.
    // https://source.corp.google.com/piper///depot/google3/devtools/crosstool/autofdo/sample_reader.cc;l=89;rcl=500000000
    if (name_set.size() == 1 && n == "[kernel.kallsyms]") {
      target_mmap_name_set = {"[kernel.kallsyms]", "[kernel.kallsyms]_text",
                              "[kernel.kallsyms]_stext"};
      path_changer = noop_path_changer;
    } else if (llvm::sys::path::is_absolute(n)) {
      target_mmap_name_set = name_set;
      path_changer = noop_path_changer;
    } else {
      for (const std::string &nm : name_set) {
        target_mmap_name_set.insert(llvm::sys::path::filename(nm).str());
      }
      path_changer = name_only_path_changer;
    }
  }

  // mmap_fn is always absolute path. If target_mmap_name is absolute, we
  // compare both directly. Otherwise, we only compare the name part of
  // mmap_fn.
  bool operator()(const std::string &mmap_fn) const {
    return target_mmap_name_set.find(path_changer(mmap_fn).str()) !=
           target_mmap_name_set.end();
  }

  std::function<llvm::StringRef(const std::string &mmap_fn)> path_changer;
  absl::flat_hash_set<std::string> target_mmap_name_set;

  static llvm::StringRef noop_path_changer(const std::string &n) { return n; }
  static llvm::StringRef name_only_path_changer(const std::string &n) {
    return llvm::sys::path::filename(n);
  }
};

absl::StatusOr<absl::flat_hash_set<std::string>> GetBuildIdNames(
    const quipper::PerfReader &perf_reader, absl::string_view build_id) {
  absl::flat_hash_set<std::string> build_id_names;
  std::vector<std::pair<std::string, std::string>> existing_build_ids;
  for (const auto &build_id_entry : perf_reader.build_ids()) {
    if (!build_id_entry.has_filename() || !build_id_entry.has_build_id_hash())
      continue;
    std::string ascii_build_id =
        BinaryDataToAscii(build_id_entry.build_id_hash());
    existing_build_ids.emplace_back(build_id_entry.filename(), ascii_build_id);
    if (ascii_build_id == build_id) {
      const std::string &filename = build_id_entry.filename();
      if (filename.empty()) continue;
      // Similar to:
      // https://source.corp.google.com/piper///depot/google3/base/elfcore.c;l=1344;bpv=0;bpt=1;rcl=502744078
      if (absl::StartsWith(filename, "/anon_hugepage") ||
          absl::StartsWith(filename, "[anon:")) {
        LOG(INFO) << "Ignoring anonymous file with matching build_id: '"
                  << filename << "'.";
        continue;
      }
      LOG(INFO) << "Found file name with matching build_id: '" << filename
                << "'.";
      build_id_names.insert(filename);
    }
  }
  if (!build_id_names.empty()) return build_id_names;

  // No build matches.
  return absl::FailedPreconditionError(absl::StrCat(
      "No file with matching buildId in perf data, which contains the "
      "following <file, build_id>:\n",
      absl::StrJoin(
          existing_build_ids, "\n",
          [](std::string *out, const std::pair<std::string, std::string> &p) {
            absl::StrAppend(out, "\t", p.first, ": ", p.second);
          })));
}

// Find the set of file names in perf.data file which has the same build id as
// found in "binary_file_name".
std::optional<absl::flat_hash_set<std::string>>
FindFileNameInPerfDataWithFileBuildId(const quipper::PerfReader &perf_reader,
                                      const std::string &binary_file_name,
                                      const BinaryContent &binary_content) {
  if (binary_content.build_id.empty()) {
    LOG(INFO) << "No Build Id found in '" << binary_file_name << "'.";
    return std::nullopt;
  }
  LOG(INFO) << "Build Id found in '" << binary_file_name
            << "': " << binary_content.build_id;
  absl::StatusOr<absl::flat_hash_set<std::string>> build_id_names =
      GetBuildIdNames(perf_reader, binary_content.build_id);

  if (!build_id_names.ok()) {
    LOG(INFO) << build_id_names.status();
    return std::nullopt;
  }

  for (const std::string &fn : *build_id_names)
    LOG(INFO) << "Build Id '" << binary_content.build_id << "' has filename '"
              << fn << "'.";
  return *build_id_names;
}

// Select mmaps from perf.data.
// a) match_mmap_names is empty && binary has build_id:
//    the perf.data mmaps are selected using binary's build_id, if there is no
//    match, select fails.
// b) match_mmap_names is empty && binary does not have build_id:
//    select fails.
// c) match_mmap_names is not empty:
//    the perf.data mmap is selected using match_mmap_name
absl::StatusOr<BinaryMMaps> SelectMMaps(
    PerfDataProvider::BufferHandle &perf_data,
    absl::Span<const absl::string_view> match_mmap_names,
    const BinaryContent &binary_content) {
  quipper::PerfReader perf_reader;
  // Ignore SAMPLE events for now to reduce memory usage. They will be needed
  // only in AggregateLBR, which will do a separate pass over the profiles.
  perf_reader.SetEventTypesToSkipWhenSerializing({quipper::PERF_RECORD_SAMPLE});
  if (!perf_reader.ReadFromPointer(perf_data.buffer->getBufferStart(),
                                   perf_data.buffer->getBufferSize())) {
    return absl::FailedPreconditionError(
        absl::StrCat("Failed to read perf data file: ", perf_data.description));
  }

  quipper::PerfParser perf_parser(&perf_reader);
  if (!perf_parser.ParseRawEvents()) {
    return absl::FailedPreconditionError(
        absl::StrCat("Failed to parse perf raw events for perf file: '",
                     perf_data.description, "'."));
  }

  std::unique_ptr<MMapSelector> mmap_selector;
  // If `match_mmap_names` is empty, we try to use build-id name in matching.
  if (match_mmap_names.empty()) {
    if (auto fn_set = FindFileNameInPerfDataWithFileBuildId(
            perf_reader, binary_content.file_name, binary_content)) {
      mmap_selector = std::make_unique<MMapSelector>(*fn_set);
    } else {
      // No filenames have been found either because the input binary
      // has no build-id or no matching build-id found in perf.data.
      if (binary_content.build_id.empty()) {
        return absl::FailedPreconditionError(
            absl::StrCat(binary_content.file_name,
                         " has no build-id. Use '--profiled_binary_name' to "
                         "force name matching."));
      }
      return absl::FailedPreconditionError(absl::StrCat(
          binary_content.file_name, " has build-id '", binary_content.build_id,
          "', however, this build-id is not found in the perf "
          "build-id list. Use '--profiled_binary_name' to force name "
          "matching."));
    }
  } else {
    mmap_selector =
        std::make_unique<MMapSelector>(absl::flat_hash_set<std::string>(
            match_mmap_names.begin(), match_mmap_names.end()));
  }

  BinaryMMaps binary_mmaps;
  for (const auto &pe : perf_parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *event_ptr = pe.event_ptr;
    if (event_ptr->event_type_case() !=
        quipper::PerfDataProto_PerfEvent::kMmapEvent)
      continue;
    const quipper::PerfDataProto_MMapEvent &mmap_evt = event_ptr->mmap_event();
    if (!mmap_evt.has_filename() || mmap_evt.filename().empty() ||
        !mmap_evt.has_start() || !mmap_evt.has_len() || !mmap_evt.has_pid())
      continue;

    if (!(*mmap_selector)(mmap_evt.filename())) continue;

    // For kernel mmap event, pid is `kKernelPid`.
    uint32_t pid = mmap_evt.pid();
    uint64_t load_addr = mmap_evt.start();
    uint64_t load_size = mmap_evt.len();
    uint64_t page_offset = mmap_evt.has_pgoff() ? mmap_evt.pgoff() : 0;

    bool entry_exists = false;
    auto existing_mmaps = binary_mmaps.find(pid);
    if (existing_mmaps != binary_mmaps.end()) {
      for (const MMapEntry &e : existing_mmaps->second) {
        if (e.load_addr == load_addr && e.load_size == load_size &&
            e.page_offset == page_offset) {
          entry_exists = true;
          continue;
        }
        if (!((load_addr + load_size <= e.load_addr) ||
              (e.load_addr + e.load_size <= load_addr))) {
          return absl::FailedPreconditionError(absl::StrCat(
              "Found conflict mmap event: ",
              MMapEntry{pid, load_addr, load_size, page_offset,
                        binary_content.file_name}
                  .DebugString(),
              ". Existing mmap entries:\n",
              absl::StrJoin(existing_mmaps->second, "\n",
                            [&](std::string *out, const MMapEntry &me) {
                              absl::StrAppend(out, "\t", me.DebugString());
                            })));
        }
      }
      if (!entry_exists)
        existing_mmaps->second.emplace(pid, load_addr, load_size, page_offset,
                                       mmap_evt.filename());
    } else {
      binary_mmaps[pid].emplace(pid, load_addr, load_size, page_offset,
                                mmap_evt.filename());
    }
  }  // End of iterating perf mmap events.

  if (binary_mmaps.empty()) {
    return absl::FailedPreconditionError(
        absl::StrCat("Failed to find any mmap entries matching: '",
                     absl::StrJoin(match_mmap_names, "' or '"), "'."));
  }

  for (const auto &[pid, mmap_entries] : binary_mmaps) {
    LOG(INFO) << absl::StrCat(
        "Found mmap: pid=", pid, "\n",
        absl::StrJoin(mmap_entries, "\n",
                      [](std::string *out, const MMapEntry &mme) {
                        return absl::StrAppend(out, "\t", mme.DebugString());
                      }));
  }
  return binary_mmaps;
}

// This function translates runtime address to symbol address:
// First of all, we find all the mmaps that have "pid", and from those to pick a
// single mmap that covers "addr".
//
// Secondly, from that mmap, we compute file_offset as "addr - mmap.load_address
// + mmap.page_offset".
//
// Thirdly, find the segment that contains file_offste, and compute symbol
// address as "file_offset - segment.offset + segment.vaddr".
uint64_t PerfDataReader::RuntimeAddressToBinaryAddress(uint32_t pid,
                                                       uint64_t addr) const {
  auto i = binary_mmaps_.find(pid);
  if (i == binary_mmaps_.end()) return kInvalidBinaryAddress;
  const MMapEntry *mmap = nullptr;
  for (const MMapEntry &p : i->second)
    if (p.load_addr <= addr && addr < p.load_addr + p.load_size) mmap = &p;
  if (!mmap) return kInvalidBinaryAddress;

  uint64_t file_offset;
  bool is_kernel = (pid == kKernelPid);
  if (!is_kernel && !binary_content_->is_pie) return addr;

  if (is_kernel) {
    // For kernel, do not use page_offset.
    file_offset = addr - mmap->load_addr;
  } else {
    file_offset = addr - mmap->load_addr + mmap->page_offset;
  }
  bool kernel_first_segment = true;
  uint64_t kernel_first_segment_offset;
  for (const auto &segment : binary_content_->segments) {
    uint64_t off;
    if (is_kernel) {
      // For kernel, use "0" as the offset for the first X-able segment.
      // And for the second or later X-able segments, we subtract the fist
      // X-able segment's offset from their segments' offset to get to the
      // "offset".
      if (kernel_first_segment) {
        off = 0;
        kernel_first_segment = false;
        kernel_first_segment_offset = segment.offset;
      } else {
        off = segment.offset - kernel_first_segment_offset;
        // We *believe* all kernel samples should come from the first executable
        // kernel segment. (The second executable segment contains
        // ".init.text" and ".exit.text", etc.)
        LOG(WARNING) << absl::StrFormat(
            "kernel runtime address 0x%lx does not come from the first "
            "executable segment",
            addr);
      }
    } else {
      off = segment.offset;
    }
    if (off <= file_offset && file_offset < off + segment.memsz) {
      return file_offset - off + segment.vaddr;
    }
  }
  LOG(WARNING) << absl::StrFormat(
      "pid: %u, virtual address: %#x belongs to '%s', file_offset=%lu, not "
      "inside any loadable segment.",
      pid, addr, mmap->file_name, file_offset);
  return kInvalidBinaryAddress;
}

void PerfDataReader::ReadWithSampleCallBack(
    absl::FunctionRef<void(const quipper::PerfDataProto::SampleEvent &)>
        callback) const {
  quipper::PerfReader perf_reader;
  // We don't need to serialise anything here, so let's exclude all major event
  // types.
  perf_reader.SetEventTypesToSkipWhenSerializing(
      {quipper::PERF_RECORD_SAMPLE, quipper::PERF_RECORD_MMAP,
       quipper::PERF_RECORD_FORK, quipper::PERF_RECORD_COMM});
  perf_reader.SetSampleCallback(callback);
  if (!perf_reader.ReadFromPointer(perf_data_.buffer->getBufferStart(),
                                   perf_data_.buffer->getBufferSize())) {
    LOG(FATAL) << "Failed to read perf data file: " << perf_data_.description;
  }
}

absl::Status PerfDataReader::ReadWithSpeRecordCallBack(
    absl::FunctionRef<void(const quipper::ArmSpeDecoder::Record &, int)>
        callback) const {
  quipper::PerfReader perf_reader;
  // We don't need to serialise anything here, so let's exclude all major event
  // types.
  perf_reader.SetEventTypesToSkipWhenSerializing(
      {quipper::PERF_RECORD_SAMPLE, quipper::PERF_RECORD_MMAP,
       quipper::PERF_RECORD_FORK, quipper::PERF_RECORD_COMM});
  if (!perf_reader.ReadFromPointer(perf_data_.buffer->getBufferStart(),
                                   perf_data_.buffer->getBufferSize())) {
    LOG(FATAL) << "Failed to read perf data file: " << perf_data_.description;
  }

  SpeTidPidProvider spe_pid_provider(perf_reader.events());
  for (const quipper::PerfDataProto_PerfEvent &event : perf_reader.events()) {
    if (!event.has_auxtrace_event()) continue;
    quipper::ArmSpeDecoder::Record record;
    quipper::ArmSpeDecoder decoder(event.auxtrace_event().trace_data(),
                                   /*is_cross_endian=*/false);
    while (decoder.NextRecord(&record)) {
      absl::StatusOr<int> pid = spe_pid_provider.GetPid(record);
      if (!pid.ok()) continue;
      callback(record, *pid);
    }
  }
  return absl::OkStatus();
}

void PerfDataReader::AggregateLBR(LbrAggregation *result) const {
  const bool is_kernel_mode = IsKernelMode();
  if (is_kernel_mode) LOG(WARNING) << "Input binary is kernel";
  ReadWithSampleCallBack([&](const quipper::PerfDataProto::SampleEvent &event) {
    uint32_t pid;
    if (is_kernel_mode) {
      // For kernel, we do not filter event by pid, we check all LBR events.
      // Because kernel branch events can exist in any process's LBR stack.
      pid = kKernelPid;
    } else {
      if (!event.has_pid() ||
          binary_mmaps_.find(event.pid()) == binary_mmaps_.end())
        return;
      pid = event.pid();
    }
    const auto &brstack = event.branch_stack();
    if (brstack.empty()) return;
    uint64_t last_from = kInvalidBinaryAddress;
    uint64_t last_to = kInvalidBinaryAddress;
    for (int p = brstack.size() - 1; p >= 0; --p) {
      const auto &be = brstack.Get(p);
      uint64_t from = RuntimeAddressToBinaryAddress(pid, be.from_ip());
      uint64_t to = RuntimeAddressToBinaryAddress(pid, be.to_ip());
      // NOTE(shenhan): LBR sometimes duplicates the first entry by mistake (*).
      // For now we treat these to be true entries.
      // (*)  (p == 0 && from == lastFrom && to == lastTo) ==> true

      ++result->branch_counters[{.from = from, .to = to}];
      if (last_to != kInvalidBinaryAddress && last_to <= from)
        ++result->fallthrough_counters[{.from = last_to, .to = from}];
      last_to = to;
      last_from = from;
    }
  });
}

absl::Status PerfDataReader::AggregateSpe(BranchFrequencies &result) const {
  const bool is_kernel_mode = IsKernelMode();
  if (is_kernel_mode) LOG(WARNING) << "Input binary is kernel";
  return ReadWithSpeRecordCallBack(
      [&](const quipper::ArmSpeDecoder::Record &record, int pid) {
        // Don't filter pid since kernel branches can be in any process's SPE
        // records.
        if (is_kernel_mode) pid = kKernelPid;

        if (binary_mmaps_.count(pid) == 0) return;
        if (!record.event.retired || !record.op.is_br_eret) return;

        uint64_t from_addr = RuntimeAddressToBinaryAddress(pid, record.ip.addr);
        // SPE records for unconditional branches are sometimes annotated as
        // `cond_not_taken`, even though the Arm Architecture Reference Manual
        // specifies otherwise. To be safe, only conditional branches should be
        // recorded as not-taken branches.
        if (record.op.br_eret.br_cond && record.event.cond_not_taken) {
          ++result.not_taken_branch_counters[{.address = from_addr}];
          return;
        }
        uint64_t to_addr =
            RuntimeAddressToBinaryAddress(pid, record.tgt_br_ip.addr);
        ++result.taken_branch_counters[{.from = from_addr, .to = to_addr}];
      });
}

bool PerfDataReader::IsKernelMode() const {
  return binary_mmaps_.count(kKernelPid) > 0;
}

absl::StatusOr<PerfDataReader> BuildPerfDataReader(
    PerfDataProvider::BufferHandle perf_data,
    const BinaryContent *binary_content, absl::string_view match_mmap_name) {
  auto match_mmap_names = absl::MakeConstSpan(
      &match_mmap_name, /*size=*/match_mmap_name.empty() ? 0 : 1);

  ASSIGN_OR_RETURN(BinaryMMaps binary_mmaps,
                   SelectMMaps(perf_data, match_mmap_names, *binary_content));
  return PerfDataReader(std::move(perf_data), std::move(binary_mmaps),
                        binary_content);
}
}  // namespace devtools_crosstool_autofdo
