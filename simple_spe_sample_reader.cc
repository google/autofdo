#include "simple_spe_sample_reader.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "base/logging.h"
#include "quipper/arm_spe_decoder.h"
#include "quipper/perf_reader.h"

namespace devtools_crosstool_autofdo {

namespace {
// Simple MMAP entry to track binary load information
struct MMapInfo {
  uint64_t load_addr;  // Runtime load address
  uint64_t load_size;  // Size of loaded region
  uint64_t page_offset;  // Page offset in the binary file
  std::string filename;
  
  // Check if an address falls within this mmap
  bool Contains(uint64_t addr) const {
    return addr >= load_addr && addr < (load_addr + load_size);
  }
  
  // Convert runtime VA to binary offset
  uint64_t VaToBinaryOffset(uint64_t va) const {
    if (!Contains(va)) return 0;
    return (va - load_addr) + page_offset;
  }
};

// Find the mmap entry for our target binary
bool FindBinaryMMap(const quipper::PerfReader& reader,
                    const std::filesystem::path& binary_path,
                    const std::string& build_id,
                    MMapInfo* mmap_info) {
  std::string base_name = binary_path.filename().string();
  
  for (const auto& event : reader.events()) {
    if (!event.has_mmap_event()) continue;
    
    const auto& mmap = event.mmap_event();
    std::string mmap_filename = std::filesystem::path(mmap.filename()).filename().string();
    
    if (mmap_filename == base_name) {
      mmap_info->load_addr = mmap.start();
      mmap_info->load_size = mmap.len();
      mmap_info->page_offset = mmap.pgoff();
      mmap_info->filename = mmap.filename();
      
      LOG(INFO) << "Found MMAP: " << base_name << " at 0x" << std::hex 
                << mmap_info->load_addr << std::dec;
      return true;
    }
  }
  return false;
}
}  // namespace

SimpleSpeDataSampleReader::SimpleSpeDataSampleReader(
    absl::string_view profile_file,
    absl::string_view binary_file)
    : FileSampleReader(profile_file),
      binary_file_(binary_file) {}

// Synthesize execution ranges from branch data for profile generation
void SimpleSpeDataSampleReader::SynthesizeRanges() {
  constexpr int kArmInstructionSize = 4;
  constexpr int kMaxRangeSize = 0x1000;
  
  // Build incoming branch weights (target → count)
  std::map<uint64_t, uint64_t> incoming_weights;
  for (const auto& [branch, count] : branch_count_map_) {
    incoming_weights[branch.second] += count;
  }
  
  if (incoming_weights.empty()) return;
  
  // Collect all branch addresses for scanning
  std::set<uint64_t> branch_addrs;
  for (const auto& [branch, _] : branch_count_map_) branch_addrs.insert(branch.first);
  for (const auto& [addr, _] : address_count_map_) branch_addrs.insert(addr);
  
  uint64_t ranges_created = 0;
  for (auto it = incoming_weights.begin(); it != incoming_weights.end(); ++it) {
    uint64_t range_start = it->first;
    uint64_t range_weight = it->second;
    uint64_t scan_end = (std::next(it) != incoming_weights.end()) 
                        ? std::next(it)->first : range_start + kMaxRangeSize;
    
    while (range_weight > 0 && range_start < scan_end) {
      auto next_branch = branch_addrs.lower_bound(range_start);
      uint64_t range_end = (next_branch != branch_addrs.end() && *next_branch < scan_end)
                           ? *next_branch : scan_end - kArmInstructionSize;
      
      if (range_end >= range_start) {
        range_count_map_[Range(range_start, range_end)] += range_weight;
        ranges_created++;
      }
      
      if (next_branch != branch_addrs.end() && *next_branch < scan_end) {
        auto not_taken_it = address_count_map_.find(*next_branch);
        range_weight = (not_taken_it != address_count_map_.end()) ? not_taken_it->second : 0;
        range_start = *next_branch + kArmInstructionSize;
      } else {
        break;
      }
    }
  }
  
  LOG(INFO) << "Synthesized " << ranges_created << " execution ranges from " 
            << incoming_weights.size() << " branch targets";
}

bool SimpleSpeDataSampleReader::Append(const std::string &profile_file) {
  // Create a temporary reader for the additional profile file
  SimpleSpeDataSampleReader temp_reader(profile_file, binary_file_.string());
  
  if (!temp_reader.Read()) {
    LOG(ERROR) << "Failed to read profile file: " << profile_file;
    return false;
  }
  
  // Merge address counts (not-taken branches)
  for (const auto& [addr, count] : temp_reader.address_count_map_) {
    address_count_map_[addr] += count;
  }
  
  // Merge branch counts (taken branches)
  for (const auto& [branch, count] : temp_reader.branch_count_map_) {
    branch_count_map_[branch] += count;
  }
  
  // Merge range counts
  for (const auto& [range, count] : temp_reader.range_count_map_) {
    range_count_map_[range] += count;
  }
  
  // Update total count
  total_count_ += temp_reader.total_count_;
  
  LOG(INFO) << "Appended profile " << profile_file 
            << " (" << temp_reader.total_count_ << " samples)";
  
  return true;
}

bool SimpleSpeDataSampleReader::Read() {
  // Read the perf data file
  quipper::PerfReader reader;
  if (!reader.ReadFile(profile_file_)) {
    LOG(ERROR) << "Failed to read perf data file: " << profile_file_;
    return false;
  }

  // Find MMAP info to convert VA to binary offsets (essential for symbol resolution)
  MMapInfo mmap_info;
  if (!FindBinaryMMap(reader, binary_file_, "", &mmap_info)) {
    LOG(ERROR) << "No MMAP event found for " << binary_file_;
    return false;
  }

  // Process AUXTRACE events containing SPE data
  const auto &events = reader.events();
  uint64_t spe_event_count = 0;
  uint64_t branch_records = 0;
  
  for (const auto &event : events) {
    // Check if this event contains ARM SPE trace data
    if (!event.has_auxtrace_event()) continue;
    
    spe_event_count++;
    
    quipper::ArmSpeDecoder decoder(event.auxtrace_event().trace_data(), false);
    quipper::ArmSpeDecoder::Record record;
    
    while (decoder.NextRecord(&record)) {
      // Only process retired branch instructions
      if (!record.event.retired || !record.op.is_br_eret) continue;
      
      branch_records++;
      uint64_t runtime_ip = record.ip.addr;
      if (runtime_ip == 0) continue;
      
      // Convert VA to binary offset for symbol resolution
      uint64_t binary_offset = mmap_info.VaToBinaryOffset(runtime_ip);
      if (binary_offset == 0) continue;  // Skip addresses outside binary
      
      // Handle conditional not-taken branches
      if (record.op.br_eret.br_cond && record.event.cond_not_taken) {
        address_count_map_[binary_offset]++;
        total_count_++;
        continue;
      }
      
      // Handle taken branches
      uint64_t target_runtime = record.tgt_br_ip.addr;
      if (target_runtime != 0) {
        uint64_t target_offset = mmap_info.VaToBinaryOffset(target_runtime);
        if (target_offset != 0) {
          branch_count_map_[Branch(binary_offset, target_offset)]++;
          total_count_++;
        }
      }
    }
  }

  if (spe_event_count == 0) {
    LOG(ERROR) << "No ARM SPE AUXTRACE events found";
    return false;
  }
  
  if (branch_records == 0 || total_count_ == 0) {
    LOG(ERROR) << "No valid branch samples collected";
    return false;
  }

  // Synthesize execution ranges (critical for profile generation)
  SynthesizeRanges();
  
  LOG(INFO) << "SPE processing complete: " << branch_records << " branches, "
            << address_count_map_.size() << " not-taken, "
            << branch_count_map_.size() << " taken, "
            << range_count_map_.size() << " ranges";
  return true;
}

}  // namespace devtools_crosstool_autofdo

