// Copyright 2011 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Read the samples from the profile datafile.

#include "sample_reader.h"

#include <inttypes.h>

#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/port.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "third_party/abseil/absl/strings/str_join.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Path.h"
#include "quipper/perf_parser.h"
#include "quipper/perf_reader.h"
#include "util/symbolize/elf_reader.h"

ABSL_FLAG(uint64_t, strip_dup_backedge_stride_limit, 0x1000,
          "Controls the limit of backedge stride hold by the heuristic "
          "to strip duplicated entries in LBR stack. ");

namespace devtools_crosstool_autofdo {

PerfDataSampleReader::PerfDataSampleReader(const string &profile_file,
                                           const string &re,
                                           const string &build_id)
    : FileSampleReader(profile_file), build_id_(build_id), re_(re.c_str()) {}

PerfDataSampleReader::~PerfDataSampleReader() {}

// Returns true if name equals any binary path in focus_bins_, or
// focus_bins_ is empty and name matches re_.
bool PerfDataSampleReader::MatchBinary(const string &name) {
  if (focus_bins_.empty()) {
    return std::regex_search(name.c_str(), re_);
  } else {
    for (const auto &binary_path : focus_bins_) {
      if (name == binary_path) {
        return true;
      }
    }
    return false;
  }
}

// Stores matching binary paths to focus_bins_ for a given build_id_.
void PerfDataSampleReader::GetFileNameFromBuildID(const quipper::PerfReader*
                                                  reader) {
  std::map<string, string> name_buildid_map;
  reader->GetFilenamesToBuildIDs(&name_buildid_map);

  focus_bins_.clear();
  for (const auto &name_buildid : name_buildid_map) {
    if (name_buildid.second == build_id_) {
      focus_bins_.insert(name_buildid.first);
      // The build ID event reports the kernel name as '[kernel.kallsyms]'.
      // However, the MMAP event reports the kernel image as either
      // '[kernel.kallsyms]_stext' or '[kernel.kallsyms]_text'.
      // If a build_id is associated with the kernel image name, include the
      // alternate names in focus_bins_ too.
      if (name_buildid.first == "[kernel.kallsyms]") {
        focus_bins_.insert("[kernel.kallsyms]_stext");
        focus_bins_.insert("[kernel.kallsyms]_text");
      }
    }
  }
  if (focus_bins_.empty()) {
    LOG(ERROR) << "Cannot find binary with buildid " << build_id_;
  }
}

std::set<uint64> SampleReader::GetSampledAddresses() const {
  std::set<uint64> addrs;
  if (range_count_map_.size() > 0) {
    for (const auto &range_count : range_count_map_) {
      addrs.insert(range_count.first.first);
    }
  } else {
    for (const auto &addr_count : address_count_map_) {
      addrs.insert(addr_count.first);
    }
  }
  return addrs;
}

uint64 SampleReader::GetSampleCountOrZero(uint64 addr) const {
  AddressCountMap::const_iterator iter = address_count_map_.find(addr);
  if (iter == address_count_map_.end())
    return 0;
  else
    return iter->second;
}

uint64 SampleReader::GetTotalSampleCount() const {
  uint64 ret = 0;

  if (range_count_map_.size() > 0) {
    for (const auto &range_count : range_count_map_) {
      ret += range_count.second;
    }
  } else {
    for (const auto &addr_count : address_count_map_) {
      ret += addr_count.second;
    }
  }
  return ret;
}

bool SampleReader::ReadAndSetTotalCount() {
  if (!Read()) {
    return false;
  }
  if (range_count_map_.size() > 0) {
    for (const auto &range_count : range_count_map_) {
      total_count_ += range_count.second * (1 + range_count.first.second -
                                            range_count.first.first);
    }
  } else {
    for (const auto &addr_count : address_count_map_) {
      total_count_ += addr_count.second;
    }
  }
  return true;
}

bool FileSampleReader::Read() {
  return Append(profile_file_);
}

bool TextSampleReaderWriter::Append(const string &profile_file) {
  FILE *fp = fopen(profile_file.c_str(), "r");
  if (fp == NULL) {
    LOG(ERROR) << "Cannot open " << profile_file << " to read";
    return false;
  }
  uint64_t num_records;

  // Reads in the range_count_map
  if (1 != fscanf(fp, "%" SCNu64 "\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64_t from, to, count;
    if (3 != fscanf(fp, "%" SCNx64 "-%" SCNx64 ":%" SCNu64 "\n", &from, &to,
                    &count)) {
      LOG(ERROR) << "Error reading from " << profile_file;
      fclose(fp);
      return false;
    }
    range_count_map_[Range(from, to)] += count;
  }

  // Reads in the addr_count_map
  if (1 != fscanf(fp, "%" SCNu64 "\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64_t addr, count;
    if (2 != fscanf(fp, "%" SCNx64 ":%" SCNu64 "\n", &addr, &count)) {
      LOG(ERROR) << "Error reading from " << profile_file;
      fclose(fp);
      return false;
    }
    address_count_map_[addr] += count;
  }

  // Reads in the branch_count_map
  if (1 != fscanf(fp, "%" SCNu64 "\n", &num_records)) {
    LOG(ERROR) << "Error reading from " << profile_file;
    fclose(fp);
    return false;
  }
  for (int i = 0; i < num_records; i++) {
    uint64_t from, to, count;
    if (3 != fscanf(fp, "%" SCNx64 "->%" SCNx64 ":%" SCNu64 "\n", &from, &to,
                    &count)) {
      LOG(ERROR) << "Error reading from " << profile_file;
      fclose(fp);
      return false;
    }
    branch_count_map_[Branch(from, to)] += count;
  }
  fclose(fp);
  return true;
}

void TextSampleReaderWriter::Merge(const SampleReader &reader) {
  for (const auto &range_count : reader.range_count_map()) {
    range_count_map_[range_count.first] += range_count.second;
  }
  for (const auto &addr_count : reader.address_count_map()) {
    address_count_map_[addr_count.first] += addr_count.second;
  }
  for (const auto &branch_count : reader.branch_count_map()) {
    branch_count_map_[branch_count.first] += branch_count.second;
  }
}

bool TextSampleReaderWriter::Write(const char *aux_info) {
  FILE *fp = fopen(profile_file_.c_str(), "w");
  if (fp == NULL) {
    LOG(ERROR) << "Cannot open " << profile_file_ << " to write";
    return false;
  }

  fprintf(fp, "%" PRIuS "\n", range_count_map_.size());
  for (const auto &range_count : range_count_map_) {
    absl::FPrintF(fp, "%x-%x:%u\n", range_count.first.first,
                  range_count.first.second, range_count.second);
  }
  fprintf(fp, "%" PRIuS "\n", address_count_map_.size());
  for (const auto &addr_count : address_count_map_) {
    absl::FPrintF(fp, "%x:%u\n", addr_count.first, addr_count.second);
  }
  fprintf(fp, "%" PRIuS "\n", branch_count_map_.size());
  for (const auto &branch_count : branch_count_map_) {
    absl::FPrintF(fp, "%x->%x:%u\n", branch_count.first.first,
                  branch_count.first.second, branch_count.second);
  }
  if (aux_info) {
    fprintf(fp, "%s", aux_info);
  }
  fclose(fp);
  return true;
}

bool TextSampleReaderWriter::IsFileExist() const {
  FILE *fp = fopen(profile_file_.c_str(), "r");
  if (fp == NULL) {
    return false;
  } else {
    fclose(fp);
    return true;
  }
}

// Given "n", compare it to each of mmap_event.filename. If "n" is absolute,
// then we compare "n" w/ mmap_event.filename. Otherwise we compare n's name
// part w/ mmap_event.filename's name part.
struct MMapSelector {
  explicit MMapSelector(const std::string &n) {
    if (llvm::sys::path::is_absolute(n)) {
      target_mmap_name = n;
      path_changer = noop_path_changer;
    } else {
      target_mmap_name = llvm::sys::path::filename(n);
      path_changer = name_only_path_changer;
    }
  }

  // mmap_fn is always absolute path. If target_mmap_name is absolute, we
  // compare both directly. Otherwise, we only compare the name part of
  // mmap_fn.
  bool operator()(const std::string &mmap_fn) const {
    return target_mmap_name == path_changer(mmap_fn);
  }

  std::function<llvm::StringRef(const std::string &mmap_fn)> path_changer;
  llvm::StringRef target_mmap_name;

  static llvm::StringRef noop_path_changer(const std::string &n) { return n; }
  static llvm::StringRef name_only_path_changer(const std::string &n) {
    return llvm::sys::path::filename(n);
  }
};

std::ostream &operator<<(std::ostream &os, const MMapEntry &me) {
  return os << std::showbase << std::hex << "[" << me.load_addr << ", "
            << (me.load_addr + me.load_size) << "] (pgoff=" << me.page_offset
            << ", size=" << me.load_size << ", fn='" << me.file_name << "')";
}

// Read loadable and executable segment information into BinaryInfo::segments.
template <class ELFT>
static bool ReadLoadableSegments(BinaryInfo *binary_info) {
  llvm::object::ELFObjectFile<ELFT> *elf_object =
      llvm::dyn_cast<llvm::object::ELFObjectFile<ELFT>,
                     llvm::object::ObjectFile>(binary_info->object_file.get());
  if (!elf_object) return false;

  const llvm::object::ELFFile<ELFT> *elf_file = elf_object->getELFFile();
  if (!elf_file) return false;

  auto program_headers = elf_file->program_headers();
  if (!program_headers) return false;

  for (const typename ELFT::Phdr &phdr : *program_headers) {
    if (phdr.p_type != llvm::ELF::PT_LOAD ||
        ((phdr.p_flags & llvm::ELF::PF_X) == 0))
      continue;
    if (phdr.p_paddr != phdr.p_vaddr) {
      LOG(ERROR) << "ELF type not supported: segment load vaddr != paddr.";
      return false;
    }
    if (phdr.p_memsz != phdr.p_filesz) {
      LOG(ERROR) << "ELF type not supported: text segment filesz != memsz.";
      return false;
    }

    binary_info->segments.push_back(
        {phdr.p_offset, phdr.p_vaddr, phdr.p_memsz});
  }
  if (binary_info->segments.empty()) {
    LOG(ERROR) << "No loadable and executable segments found in '"
               << binary_info->file_name << "'.";
    return false;
  }
  return true;
}

// Initialize BinaryInfo object:
//  - setup file content memory buffer
//  - setup object file pointer
//  - setup "PIE" bit
//  - read loadable and executable segments
bool PerfDataReader::SelectBinaryInfo(const std::string &binary_file_name,
                                      BinaryInfo *binary_info) const {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> file =
      llvm::MemoryBuffer::getFile(binary_file_name);
  if (!file) {
    LOG(ERROR) << "Failed to read file '" << binary_file_name << "'.";
    return false;
  }
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> obj =
      llvm::object::ObjectFile::createELFObjectFile(
          llvm::MemoryBufferRef(*(*file)));
  if (!obj) {
    LOG(ERROR) << "Not a valid ELF file '" << binary_file_name << "'.";
    return false;
  }
  llvm::object::ELFObjectFileBase *elf_obj =
      llvm::dyn_cast<llvm::object::ELFObjectFileBase, llvm::object::ObjectFile>(
          (*obj).get());
  if (!elf_obj) {
    LOG(ERROR) << "Not a valid ELF file '" << binary_file_name << ".";
    return false;
  }
  binary_info->file_name = binary_file_name;
  binary_info->file_content = std::move(*file);
  binary_info->object_file = std::move(*obj);
  binary_info->is_pie = (elf_obj->getEType() == llvm::ELF::ET_DYN);
  binary_info->build_id = util::ElfReader(binary_file_name).GetBuildId();
  LOG(INFO) << "'" << binary_file_name << "' is PIE: " << binary_info->is_pie;
  if (!binary_info->build_id.empty())
    LOG(INFO) << "Build Id found in '" << binary_file_name
              << "': " << binary_info->build_id;

  const char *elf_start = binary_info->file_content->getBufferStart();
  if (binary_info->file_content->getBufferSize() <=
          strlen(llvm::ELF::ElfMagic) ||
      strncmp(elf_start, llvm::ELF::ElfMagic, strlen(llvm::ELF::ElfMagic))) {
    LOG(ERROR) << "Not a valid ELF file '" << binary_info << ".";
    return false;
  }
  const char elf_class = elf_start[4];
  const char elf_data = elf_start[5];
  if (elf_class == llvm::ELF::ELFCLASS32 &&
      elf_data == llvm::ELF::ELFDATA2LSB) {
    return ReadLoadableSegments<llvm::object::ELF32LE>(binary_info);
  } else if (elf_class == llvm::ELF::ELFCLASS32 &&
             elf_data == llvm::ELF::ELFDATA2MSB) {
    return ReadLoadableSegments<llvm::object::ELF32BE>(binary_info);
  } else if (elf_class == llvm::ELF::ELFCLASS64 &&
             elf_data == llvm::ELF::ELFDATA2LSB) {
    return ReadLoadableSegments<llvm::object::ELF64LE>(binary_info);
  } else if (elf_class == llvm::ELF::ELFCLASS64 &&
             elf_data == llvm::ELF::ELFDATA2MSB) {
    return ReadLoadableSegments<llvm::object::ELF64BE>(binary_info);
  }
  LOG(ERROR) << "Unrecognized ELF file data '" << binary_info << ".";
  return false;
}

bool PerfDataReader::SelectPerfInfo(const std::string &perf_file,
                                    const std::string &match_mmap_name,
                                    BinaryPerfInfo *binary_perf_info) const {
  // "binary_info" must already be initialized.
  if (!(binary_perf_info->binary_info.file_content)) return false;
  auto perf_reader = std::make_unique<quipper::PerfReader>();
  if (!perf_reader->ReadFile(perf_file)) {
    LOG(ERROR) << "Failed to read perf data file: " << perf_file;
    return false;
  }

  auto perf_parser = std::make_unique<quipper::PerfParser>(perf_reader.get());
  if (!perf_parser->ParseRawEvents()) {
    LOG(ERROR) << "Failed to parse perf raw events for perf file: '"
               << perf_file << "'.";
    return false;
  }

  binary_perf_info->perf_reader = std::move(perf_reader);
  binary_perf_info->perf_parser = std::move(perf_parser);

  return SelectMMaps(binary_perf_info, match_mmap_name);
}

// Find a file name in perf.data file which has the same build id as found in
// "binary_file_name".
llvm::Optional<std::string> FindFileNameInPerfDataWithFileBuildId(
    const std::string &binary_file_name, BinaryPerfInfo *info) {
  if (info->binary_info.build_id.empty()) {
    LOG(INFO) << "No Build Id found in '" << binary_file_name << "'.";
    return llvm::None;
  }
  LOG(INFO) << "Build Id found in '" << binary_file_name
            << "': " << info->binary_info.build_id;
  std::set<std::string> buildid_names;
  if (PerfDataReader().GetBuildIdNames(
          *(info->perf_reader), info->binary_info.build_id, &buildid_names)) {
    CHECK_EQ(buildid_names.size(), 1);
    const std::string fn = *(buildid_names.begin());
    LOG(INFO) << "Build Id '" << info->binary_info.build_id
              << "' has filename '" << fn << "'.";
    return fn;
  }
  return llvm::None;
}

bool PerfDataReader::SelectMMaps(BinaryPerfInfo *info,
                                 const std::string &match_mmap_name) const {
  std::string match_fn = match_mmap_name;
  // If match_mmap_name is "", we try to use build-id name in matching.
  if (match_mmap_name.empty()) {
    match_fn = info->binary_info.file_name;
    if (auto fn = FindFileNameInPerfDataWithFileBuildId(
            info->binary_info.file_name, info)) {
      match_fn = *fn;
    }
  }

  for (const auto &pe : info->perf_parser->parsed_events()) {
    quipper::PerfDataProto_PerfEvent *event_ptr = pe.event_ptr;
    if (event_ptr->event_type_case() !=
        quipper::PerfDataProto_PerfEvent::kMmapEvent)
      continue;
    const quipper::PerfDataProto_MMapEvent &mmap_evt = event_ptr->mmap_event();
    if (!mmap_evt.has_filename() || mmap_evt.filename().empty() ||
        !mmap_evt.has_start() || !mmap_evt.has_len() || !mmap_evt.has_pid())
      continue;

    MMapSelector mmap_selector(match_fn);
    if (!mmap_selector(mmap_evt.filename())) continue;

    uint64_t load_addr = mmap_evt.start();
    uint64_t load_size = mmap_evt.len();
    uint64_t page_offset = mmap_evt.has_pgoff() ? mmap_evt.pgoff() : 0;

    bool entry_exists = false;
    auto existing_mmaps = info->binary_mmaps.find(mmap_evt.pid());
    if (existing_mmaps != info->binary_mmaps.end()) {
      for (const MMapEntry &e : existing_mmaps->second) {
        if (e.load_addr == load_addr && e.load_size == load_size &&
            e.page_offset == page_offset) {
          entry_exists = true;
          continue;
        }
        if (!((load_addr + load_size <= e.load_addr) ||
              (e.load_addr + e.load_size <= load_addr))) {
          std::stringstream ss;
          ss << "Found conflict mmap event: "
             << MMapEntry{load_addr, load_size, page_offset,
                          info->binary_info.file_name}
             << ". Existing mmap entries: " << std::endl;
          for (auto &me : existing_mmaps->second) ss << "\t" << me << std::endl;
          LOG(ERROR) << ss.str();
          return false;
        }
      }
      if (!entry_exists)
        existing_mmaps->second.emplace(load_addr, load_size, page_offset,
                                       info->binary_info.file_name);
    } else {
      info->binary_mmaps[mmap_evt.pid()].emplace(
          load_addr, load_size, page_offset, info->binary_info.file_name);
    }
  }  // End of iterating perf mmap events.

  if (info->binary_mmaps.empty()) {
    LOG(ERROR) << "Failed to find any mmap entries matching: '" << match_fn
               << "'.";
    return false;
  }
  for (auto &mpid : info->binary_mmaps) {
    std::stringstream ss;
    ss << "Found mmap: pid=" << std::noshowbase << std::dec << mpid.first
       << std::endl;
    for (auto &mm : mpid.second) ss << "\t" << mm << std::endl;
    LOG(INFO) << ss.str();
  }
  return true;
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
uint64_t PerfDataReader::RuntimeAddressToBinaryAddress(
    uint64_t pid, uint64_t addr, const BinaryPerfInfo &bpi) const {
  auto i = bpi.binary_mmaps.find(pid);
  if (i == bpi.binary_mmaps.end()) return kInvalidAddress;
  const MMapEntry *mmap = nullptr;
  for (const MMapEntry &p : i->second)
    if (p.load_addr <= addr && addr < p.load_addr + p.load_size) mmap = &p;
  if (!mmap) return kInvalidAddress;
  if (!bpi.binary_info.is_pie) return addr;

  uint64_t file_offset = addr - mmap->load_addr + mmap->page_offset;
  for (const auto &segment : bpi.binary_info.segments) {
    if (segment.offset <= file_offset &&
        file_offset < segment.offset + segment.memsz) {
      return file_offset - segment.offset + segment.vaddr;
    }
  }
  LOG(WARNING) << absl::StrFormat(
      "pid: %u, virtual address: %#x belongs to '%s', file_offset=%lu, not "
      "inside any loadable segment.",
      pid, addr, mmap->file_name, file_offset);
  return kInvalidAddress;
}

void PerfDataReader::AggregateLBR(const BinaryPerfInfo &binary_perf_info,
                                  LBRAggregation *result) const {
  for (const auto &pe : binary_perf_info.perf_parser->parsed_events()) {
    quipper::PerfDataProto_PerfEvent *event_ptr = pe.event_ptr;
    if (event_ptr->event_type_case() !=
        quipper::PerfDataProto_PerfEvent::kSampleEvent)
      continue;
    auto &event = event_ptr->sample_event();
    if (!event.has_pid() || binary_perf_info.binary_mmaps.find(event.pid()) ==
                                binary_perf_info.binary_mmaps.end())
      continue;

    uint64_t pid = event.pid();
    const auto &brstack = event.branch_stack();
    if (brstack.empty()) continue;
    uint64_t last_from = kInvalidAddress;
    uint64_t last_to = kInvalidAddress;
    for (int p = brstack.size() - 1; p >= 0; --p) {
      const auto &be = brstack.Get(p);
      uint64_t from =
          RuntimeAddressToBinaryAddress(pid, be.from_ip(), binary_perf_info);
      uint64_t to =
          RuntimeAddressToBinaryAddress(pid, be.to_ip(), binary_perf_info);
      // NOTE(shenhan): LBR sometimes duplicates the first entry by mistake (*).
      // For now we treat these to be true entries.
      // (*)  (p == 0 && from == lastFrom && to == lastTo) ==> true

      result->branch_counters[std::make_pair(from, to)]++;
      if (last_to != kInvalidAddress && last_to <= from)
        result->fallthrough_counters[std::make_pair(last_to, from)]++;
      last_to = to;
      last_from = from;
    }  // End of iterating one br record
  }    // End of iterating all br records.
}

bool PerfDataReader::GetBuildIdNames(const quipper::PerfReader &perf_reader,
                                     const std::string &buildid,
                                     std::set<std::string> *buildid_names) {
  buildid_names->clear();
  std::list<std::pair<string, string>> existing_build_ids;
  for (const auto &buildid_entry : perf_reader.build_ids()) {
    if (!buildid_entry.has_filename() || !buildid_entry.has_build_id_hash())
      continue;
    string perf_build_id = buildid_entry.build_id_hash();
    string ascii_build_id = BinaryDataToAscii(perf_build_id);
    existing_build_ids.emplace_back(buildid_entry.filename(), ascii_build_id);
    if (ascii_build_id == buildid) {
      LOG(INFO) << "Found file name with matching buildid: '"
                << buildid_entry.filename() << "'.";
      buildid_names->insert(buildid_entry.filename());
    }
  }
  if (!buildid_names->empty()) return true;

  // No build matches.
  std::stringstream ss;
  ss << "No file with matching buildId in perf data, which contains the "
        "following <file, buildid>:"
     << std::endl;
  for (auto &p : existing_build_ids)
    ss << "\t" << p.first << ": " << p.second << std::endl;
  LOG(INFO) << ss.str();
  buildid_names->clear();
  return false;
}

bool PerfDataSampleReader::Append(const string &profile_file) {
  quipper::PerfReader reader;
  quipper::PerfParser parser(&reader);
  if (!reader.ReadFile(profile_file) || !parser.ParseRawEvents()) {
    return false;
  }

  // If we can find build_id from binary, and the exact build_id was found
  // in the profile, then we use focus_bins to match samples. Otherwise,
  // focus_binary_re_ is used to match the binary name with the samples.
  if (build_id_ != "") {
    GetFileNameFromBuildID(&reader);
    if (focus_bins_.empty())
      return true;
  } else {
    LOG(ERROR) << "No buildid found in binary";
  }

  for (const auto &event : parser.parsed_events()) {
    if (!event.event_ptr ||
        event.event_ptr->header().type() != quipper::PERF_RECORD_SAMPLE) {
      continue;
    }
    if (MatchBinary(event.dso_and_offset.dso_name())) {
      address_count_map_[event.dso_and_offset.offset()]++;
    }
    if (event.branch_stack.size() > 0 &&
        MatchBinary(event.branch_stack[0].to.dso_name()) &&
        MatchBinary(event.branch_stack[0].from.dso_name())) {
      branch_count_map_[Branch(event.branch_stack[0].from.offset(),
                               event.branch_stack[0].to.offset())]++;
    }
    for (int i = 1; i < event.branch_stack.size(); i++) {
      if (!MatchBinary(event.branch_stack[i].to.dso_name())) {
        continue;
      }

      // TODO(b/62827958): Get rid of this temporary workaround once the issue
      // of duplicate entries in LBR is resolved. It only happens at the head
      // of the LBR. In addition, it is possible that the duplication is
      // legitimate if there's self loop. However, it's very rare to have basic
      // blocks larger than 0x1000 formed by such self loop. So, we're ignoring
      // duplication when the resulting basic block is larger than 0x1000 (the
      // default value of FLAGS_strip_dup_backedge_stride_limit).
      if (i == 1 &&
          (event.branch_stack[0].from.offset() ==
           event.branch_stack[1].from.offset()) &&
          (event.branch_stack[0].to.offset() ==
           event.branch_stack[1].to.offset()) &&
          (event.branch_stack[0].from.offset() -
               event.branch_stack[0].to.offset() >
           absl::GetFlag(FLAGS_strip_dup_backedge_stride_limit)))
        continue;
      uint64 begin = event.branch_stack[i].to.offset();
      uint64 end = event.branch_stack[i - 1].from.offset();
      // The interval between two taken branches should not be too large.
      if (end < begin || end - begin > (1 << 20)) {
        LOG(WARNING) << "Bogus LBR data: " << begin << "->" << end;
        continue;
      }
      range_count_map_[Range(begin, end)]++;
      if (MatchBinary(event.branch_stack[i].from.dso_name())) {
        branch_count_map_[Branch(event.branch_stack[i].from.offset(),
                                 event.branch_stack[i].to.offset())]++;
      }
    }
  }
  return true;
}

std::string BinaryDataToAscii(const string &data) {
  string ascii(data.size() * 2, 0);
  const char heximal[] = "0123456789abcdef";
  for (int i = 0; i < data.size(); ++i) {
    uint8_t d(data[i]);
    ascii[i * 2] = heximal[((d >> 4) & 0xf)];
    ascii[i * 2 + 1] = heximal[(d & 0xf)];
  }
  return ascii;
}

}  // namespace devtools_crosstool_autofdo
