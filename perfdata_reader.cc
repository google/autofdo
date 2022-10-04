#include "perfdata_reader.h"

#include <functional>
#include <list>
#include <string>
#include <utility>

#include "llvm_propeller_perf_data_provider.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "quipper/perf_parser.h"
#include "quipper/perf_reader.h"

namespace {
// Convert binary data stored in data[...] into text representation.
std::string BinaryDataToAscii(const std::string &data) {
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

using ::llvm::object::BBAddrMap;

// TODO(shenhan): remove the following code once it is upstreamed.
template <class ELFT>
std::string ELFFileUtil<ELFT>::GetBuildId() {
  if (!elf_file_) return "";
  auto hex_to_char = [](uint8_t v) -> char {
    if (v < 10) return '0' + v;
    return 'a' + (v - 10);
  };
  std::vector<std::string> build_ids;
  for (const typename ELFT::Shdr &shdr :
       llvm::cantFail(elf_file_->sections())) {
    llvm::Expected<llvm::StringRef> section_name =
        elf_file_->getSectionName(shdr);
    if (!section_name || shdr.sh_type != llvm::ELF::SHT_NOTE ||
        *section_name != kBuildIDSectionName)
      continue;
    llvm::Error err = llvm::Error::success();
    for (const typename ELFT::Note &note : elf_file_->notes(shdr, err)) {
      llvm::StringRef r = note.getName();
      if (r == kBuildIdNoteName) {
        llvm::ArrayRef<uint8_t> build_id = note.getDesc();
        std::string build_id_str(build_id.size() * 2, '0');
        int k = 0;
        for (uint8_t t : build_id) {
          build_id_str[k++] = hex_to_char((t >> 4) & 0xf);
          build_id_str[k++] = hex_to_char(t & 0xf);
        }
        build_ids.push_back(std::move(build_id_str));
      }
    }
    if (errorToBool(std::move(err)))
      LOG(WARNING) << "error happened iterating note entries in '"
                   << section_name->str() << "'";
  }
  if (build_ids.empty()) return "";
  if (build_ids.size() > 1) {
    LOG(WARNING) << "more than 1 build id entries found in the binary, only "
                    "the first one will be returned";
  }
  return build_ids.front();
}

template <class ELFT>
bool ELFFileUtil<ELFT>::ReadLoadableSegments(
    devtools_crosstool_autofdo::BinaryInfo *binary_info) {
  if (!elf_file_) return false;
  auto program_headers = elf_file_->program_headers();
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

std::unique_ptr<ELFFileUtilBase> CreateELFFileUtil(
    llvm::object::ObjectFile *object_file) {
  if (!object_file) return nullptr;
  llvm::StringRef content = object_file->getData();
  const char *elf_start = content.data();

  if (content.size() <= strlen(llvm::ELF::ElfMagic) ||
      strncmp(elf_start, llvm::ELF::ElfMagic, strlen(llvm::ELF::ElfMagic))) {
    LOG(ERROR) << "Not a valid ELF file.";
    return nullptr;
  }
  const char elf_class = elf_start[llvm::ELF::EI_CLASS];
  const char elf_data = elf_start[llvm::ELF::EI_DATA];
  if (elf_class == llvm::ELF::ELFCLASS32 &&
      elf_data == llvm::ELF::ELFDATA2LSB) {
    return std::make_unique<ELFFileUtil<llvm::object::ELF32LE>>(object_file);
  } else if (elf_class == llvm::ELF::ELFCLASS32 &&
             elf_data == llvm::ELF::ELFDATA2MSB) {
    return std::make_unique<ELFFileUtil<llvm::object::ELF32BE>>(object_file);
  } else if (elf_class == llvm::ELF::ELFCLASS64 &&
             elf_data == llvm::ELF::ELFDATA2LSB) {
    return std::make_unique<ELFFileUtil<llvm::object::ELF64LE>>(object_file);
  } else if (elf_class == llvm::ELF::ELFCLASS64 &&
             elf_data == llvm::ELF::ELFDATA2MSB) {
    return std::make_unique<ELFFileUtil<llvm::object::ELF64BE>>(object_file);
  }
  LOG(ERROR) << "Unrecognized ELF file data.";
  return nullptr;
}

// Given "n", compare it to each of mmap_event.filename. If "n" is absolute,
// then we compare "n" w/ mmap_event.filename. Otherwise we compare n's name
// part w/ mmap_event.filename's name part.
struct MMapSelector {
  explicit MMapSelector(const std::set<std::string> &name_set) {
    std::string n = *(name_set.begin());
    if (llvm::sys::path::is_absolute(n)) {
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
  std::set<std::string> target_mmap_name_set;

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
    LOG(ERROR) << "Failed to read file '" << binary_file_name
               << "': " << file.getError().message();
    return false;
  }
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> obj =
      llvm::object::ObjectFile::createELFObjectFile(
          llvm::MemoryBufferRef(*(*file)));
  if (!obj) {
    std::string error_message;
    llvm::raw_string_ostream raw_string_ostream(error_message);
    raw_string_ostream << obj.takeError();
    LOG(ERROR) << "Not a valid ELF file '" << binary_file_name
               << "': " << raw_string_ostream.str();
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
  LOG(INFO) << "'" << binary_file_name << "' is PIE: " << binary_info->is_pie;

  std::unique_ptr<ELFFileUtilBase> elf_file_util =
      CreateELFFileUtil(binary_info->object_file.get());
  if (!elf_file_util)
    return false;
  binary_info->build_id = elf_file_util->GetBuildId();
  if (!binary_info->build_id.empty())
    LOG(INFO) << "Build Id found in '" << binary_file_name
              << "': " << binary_info->build_id;

  return elf_file_util->ReadLoadableSegments(binary_info);
}
bool PerfDataReader::SelectPerfInfo(PerfDataProvider::BufferHandle perf_data,
                                    const std::string &match_mmap_name,
                                    BinaryPerfInfo *binary_perf_info) const {
  // "binary_info" must already be initialized.
  if (!(binary_perf_info->binary_info.file_content)) return false;

  quipper::PerfReader perf_reader;
  // Ignore SAMPLE events for now to reduce memory usage. They will be needed
  // only in AggregateLBR, which will do a separate pass over the profiles.
  perf_reader.SetEventTypesToSkipWhenSerializing({quipper::PERF_RECORD_SAMPLE});
  if (!perf_reader.ReadFromPointer(perf_data.buffer->getBufferStart(),
                                   perf_data.buffer->getBufferSize())) {
    LOG(ERROR) << "Failed to read perf data file: " << perf_data.description;
    return false;
  }

  quipper::PerfParser perf_parser(&perf_reader);
  if (!perf_parser.ParseRawEvents()) {
    LOG(ERROR) << "Failed to parse perf raw events for perf file: '"
               << perf_data.description << "'.";
    return false;
  }

  binary_perf_info->perf_data = std::move(perf_data);

  return SelectMMaps(binary_perf_info, perf_reader, perf_parser,
                     match_mmap_name);
}

// Find the set of file names in perf.data file which has the same build id as
// found in "binary_file_name".
llvm::Optional<std::set<std::string>> FindFileNameInPerfDataWithFileBuildId(
    const std::string &binary_file_name, const quipper::PerfReader &perf_reader,
    BinaryPerfInfo *info) {
  if (info->binary_info.build_id.empty()) {
    LOG(INFO) << "No Build Id found in '" << binary_file_name << "'.";
    return llvm::None;
  }
  LOG(INFO) << "Build Id found in '" << binary_file_name
            << "': " << info->binary_info.build_id;
  std::set<std::string> buildid_names;
  if (PerfDataReader().GetBuildIdNames(perf_reader, info->binary_info.build_id,
                                       &buildid_names)) {
    for (const std::string &fn : buildid_names)
      LOG(INFO) << "Build Id '" << info->binary_info.build_id
                << "' has filename '" << fn << "'.";
    return buildid_names;
  }
  return llvm::None;
}

// Select mmaps from perf.data.
// a) match_mmap_name == "" && binary has buildid:
//    the perf.data mmaps are selected using binary's buildid, if there is no
//    match, select fails.
// b) match_mmap_name == "" && binary does not has buildid:
//    select fails.
// c) match_mmap_name != ""
//    the perf.data mmap is selected using match_mmap_name
bool PerfDataReader::SelectMMaps(BinaryPerfInfo *info,
                                 const quipper::PerfReader &perf_reader,
                                 const quipper::PerfParser &perf_parser,
                                 const std::string &match_mmap_name) const {
  std::unique_ptr<MMapSelector> mmap_selector;
  const std::string &match_fn = match_mmap_name;
  // If match_mmap_name is "", we try to use build-id name in matching.
  if (match_mmap_name.empty()) {
    if (auto fn_set = FindFileNameInPerfDataWithFileBuildId(
            info->binary_info.file_name, perf_reader, info)) {
      mmap_selector = std::make_unique<MMapSelector>(*fn_set);
    } else {
      // No filenames have been found either because the input binary
      // has no build-id or no matching build-id found in perf.data.
      if (info->binary_info.build_id.empty()) {
        LOG(ERROR)
            << info->binary_info.file_name << " has no build-id. "
            << "Use '--profiled_binary_name' to force name matching.";
        return false;
      }
      LOG(ERROR)
            << info->binary_info.file_name << " has build-id '"
            << info->binary_info.build_id
            << "', however, this build-id is not found in the perf "
               "build-id list. Use '--profiled_binary_name' to force name "
               "matching.";
      return false;
    }
  } else {
    mmap_selector = std::make_unique<MMapSelector>(
        std::set<std::string>({match_mmap_name}));
  }

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
                                       mmap_evt.filename());
    } else {
      info->binary_mmaps[mmap_evt.pid()].emplace(
          load_addr, load_size, page_offset, mmap_evt.filename());
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
  auto process_event = [&](const quipper::PerfDataProto::SampleEvent &event) {
    if (!event.has_pid() || binary_perf_info.binary_mmaps.find(event.pid()) ==
                                binary_perf_info.binary_mmaps.end())
      return;

    uint64_t pid = event.pid();
    const auto &brstack = event.branch_stack();
    if (brstack.empty()) return;
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
    }
  };

  quipper::PerfReader perf_reader;
  // We don't need to serialise anything here, so let's exclude all major event
  // types.
  perf_reader.SetEventTypesToSkipWhenSerializing(
      {quipper::PERF_RECORD_SAMPLE, quipper::PERF_RECORD_MMAP,
       quipper::PERF_RECORD_FORK, quipper::PERF_RECORD_COMM});
  perf_reader.SetSampleCallback(process_event);
  CHECK(binary_perf_info.perf_data.has_value());
  if (!perf_reader.ReadFromPointer(
          binary_perf_info.perf_data->buffer->getBufferStart(),
          binary_perf_info.perf_data->buffer->getBufferSize())) {
    LOG(FATAL) << "Failed to read perf data file: "
               << binary_perf_info.perf_data->description;
  }
}

bool PerfDataReader::GetBuildIdNames(const quipper::PerfReader &perf_reader,
                                     const std::string &buildid,
                                     std::set<std::string> *buildid_names) {
  buildid_names->clear();
  std::list<std::pair<std::string, std::string>> existing_build_ids;
  for (const auto &buildid_entry : perf_reader.build_ids()) {
    if (!buildid_entry.has_filename() || !buildid_entry.has_build_id_hash())
      continue;
    const std::string &perf_build_id = buildid_entry.build_id_hash();
    std::string ascii_build_id = BinaryDataToAscii(perf_build_id);
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
}  // namespace devtools_crosstool_autofdo
