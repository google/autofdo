// Class to represent the symbol map.

#include "symbol_map.h"

#include <elf.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "addr2line.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/container/node_hash_map.h"
#include "third_party/abseil/absl/debugging/internal/demangle.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/memory/memory.h"
#include "third_party/abseil/absl/strings/match.h"
#include "third_party/abseil/absl/strings/str_format.h"
#include <regex>
#include "util/symbolize/elf_reader.h"

#if defined(HAVE_LLVM)
#include "llvm/ADT/StringSet.h"
#endif

ABSL_FLAG(int32_t, dump_cutoff_percent, 2,
          "functions that has total count lower than this percentage of "
          "the max function count will not show in the dump");
ABSL_FLAG(double, sample_threshold_frac, 0.0,
              "Sample threshold ratio. The threshold of total function count"
              " is determined by max_sample_count * sample_threshold_frac.");
ABSL_FLAG(std::string, suffix_elision_policy, "selected",
          "Policy for eliding/merging profile entries for symbols with "
          "suffixed names: one of 'all' [default], 'selected', 'none'.");
ABSL_FLAG(bool, demangle_symbol_names, false,
          "Demangle symbol names in function profile diff output");
ABSL_FLAG(bool, use_discriminator_encoding, false,
          "Tell the symbol map that the discriminator encoding is enabled in "
          "the profile.");
ABSL_FLAG(bool, use_discriminator_multiply_factor, true,
          "Tell the symbol map whether to use discriminator multiply factors.");
#if defined(HAVE_LLVM)
ABSL_FLAG(bool, use_fs_discriminator, false,
          "Tell the symbol map whether to use FS discriminators.");
#endif

namespace {
// Prints some blank space for identation.
void Identation(int ident) {
  for (int i = 0; i < ident; i++) {
    printf(" ");
  }
}

using devtools_crosstool_autofdo::SourceInfo;

void PrintSourceLocation(uint32_t start_line, uint64_t offset, int ident) {
  Identation(ident);
  uint32_t line = SourceInfo::GetLineNumberFromOffset(offset);
  uint32_t discriminator = SourceInfo::GetDiscriminatorFromOffset(offset);
  if (discriminator) {
    printf("%u.%u: ", line + start_line, discriminator);
  } else {
    printf("%u: ", line + start_line);
  }
}

static const char *selectedSuffixes[] = {".cold", ".llvm.", ".lto_priv.", ".part.", ".isra."};

std::string getPrintName(const char *name) {
  char tmp_buf[1024];
  if (!absl::GetFlag(FLAGS_demangle_symbol_names)) return name;
  if (!absl::debugging_internal::Demangle(name, tmp_buf, sizeof(tmp_buf))) {
    LOG(WARNING) << "Demangle failed: " << std::string(name);
    return "";
  }
  return tmp_buf;
}
}  // namespace

namespace devtools_crosstool_autofdo {
ProfileInfo& ProfileInfo::operator+=(const ProfileInfo &s) {
  count += s.count;
  num_inst += s.num_inst;
  for (const auto &target_count : s.target_map) {
    target_map[target_count.first] += target_count.second;
  }
  return *this;
}

struct TargetCountCompare {
  bool operator()(const TargetCountPair &t1, const TargetCountPair &t2) const {
    if (t1.second != t2.second) {
      return t1.second > t2.second;
    } else {
      return t1.first > t2.first;
    }
  }
};

void GetSortedTargetCountPairs(const CallTargetCountMap &call_target_count_map,
                               TargetCountPairs *target_counts) {
  for (const auto &name_count : call_target_count_map) {
    target_counts->push_back(name_count);
  }
  std::sort(target_counts->begin(), target_counts->end(), TargetCountCompare());
}

bool SymbolMap::IsLLVMCompiler(const std::string &path) {
  // llvm-optout will not be in this string so we don't need to look for it
  return absl::StrContains(path, "-llvm-");
}

Symbol::~Symbol() {
  for (auto &callsite_symbol : callsites) {
    delete callsite_symbol.second;
  }
}

void Symbol::Merge(const Symbol *other) {
  total_count += other->total_count;
  head_count += other->head_count;
  if (info.file_name.empty()) {
      info.file_name = other->info.file_name;
      info.dir_name = other->info.dir_name;
  }
  for (const auto &pos_count : other->pos_counts)
    pos_counts[pos_count.first] += pos_count.second;
  // Traverses all callsite, recursively Merge the callee symbol.
  for (const auto &callsite_symbol : other->callsites) {
    std::pair<CallsiteMap::iterator, bool> ret = callsites.insert(
        CallsiteMap::value_type(callsite_symbol.first, NULL));
    // If the callsite does not exist in the current symbol, create a
    // new callee symbol with the clone's function name.
    if (ret.second) {
      ret.first->second = new Symbol();
      ret.first->second->info.func_name = ret.first->first.second;
    }
    ret.first->second->Merge(callsite_symbol.second);
  }
}

struct CallsiteLessThan {
  bool operator()(const Callsite& c1, const Callsite& c2) const {
    if (c1.first != c2.first)
      return c1.first < c2.first;
    if ((c1.second == NULL || c2.second == NULL))
      return c1.second == NULL;
    return strcmp(c1.second, c2.second) < 0;
  }
};

void Symbol::EstimateHeadCount() {
  if (head_count != 0)
    return;

  // Get the count of the source location with the lowest offset.
  uint64_t offset = std::numeric_limits<uint64_t>::max();
  std::vector<uint64_t> positions;
  for (const auto &pos_count : pos_counts)
    positions.push_back(pos_count.first);
  if (!positions.empty()) {
    uint64_t minpos = *std::min_element(positions.begin(), positions.end());
    PositionCountMap::const_iterator ret = pos_counts.find(minpos);
    DCHECK(ret != pos_counts.end());
    head_count = ret->second.count;
    offset = ret->first;
  }

  // Get the count of the callsite location with the lowest offset.
  std::vector<Callsite> calls;
  for (const auto &pos_symbol : callsites) {
    calls.push_back(pos_symbol.first);
  }
  if (!calls.empty()) {
    Callsite earliestCallsite =
        *std::min_element(calls.begin(), calls.end(), CallsiteLessThan());
    CallsiteMap::const_iterator ret = callsites.find(earliestCallsite);
    DCHECK(ret != callsites.end());
    if (ret->first.first >= offset)
      return;

    ret->second->EstimateHeadCount();
    head_count = ret->second->head_count;
  }
}

void Symbol::FlattenCallsite(uint64_t offset, const Symbol *callee) {
  pos_counts[offset].count = std::max(pos_counts[offset].count,
                                      callee->head_count);
  pos_counts[offset].target_map[callee->info.func_name] +=
      callee->head_count;
}

void Symbol::FlatMerge(const Symbol *src) {
  uint64_t src_total_count = 0;
  for (const auto &pos_count : src->pos_counts) {
    pos_counts[pos_count.first] += pos_count.second;
    src_total_count += pos_count.second.count;
  }
  total_count += src_total_count;
  head_count += src->head_count;
}

void SymbolMap::initSuffixElisionPolicy() {
  set_suffix_elision_policy(absl::GetFlag(FLAGS_suffix_elision_policy));
}

const std::string SymbolMap::suffix_elision_policy() const {
  switch (suffix_elision_policy_) {
    case ElideAll: return "all";
    case ElideSelected: return "selected";
    case ElideNone: return "none";
    default:
      LOG(FATAL) << "internal error: unknown suffix elision policy";
      return "<unknown>";
  }
}

// Suffix elision policy can take on one of three values:
//
//     all      ->   look for the first '.' in the string
//                   and remove that '.' and everything following
//
//     selected  ->  look for a specific set of suffixes including
//                   ".isra.", ".part.", ".llvm.". If one of these
//                   suffixes are found (or a combination), trim off
//                   that portion of the name. Search for a suffix
//                   starts at the end of the string and works back.
//
//     none      ->  skip suffix removal entirely (debugging)
//
void SymbolMap::set_suffix_elision_policy(const std::string &policy) {
  if (policy == "all")
    suffix_elision_policy_ = ElideAll;
  else if (policy == "selected")
    suffix_elision_policy_ = ElideSelected;
  else if (policy == "none")
    suffix_elision_policy_ = ElideNone;
  else
    LOG(FATAL) << "suffix elision policy " << policy << " not supported.";
}

// Strip the suffix literally and keep the parts before and after the
// suffix unchanged. For example, to strip a suffix like ".cold", what
// we want is to erase ".cold" from input but keep the rest unchanged.
static bool StripLastOccurrenceOf(std::string &input,
                                  const std::string &suffix) {
  auto it = input.rfind(suffix);
  if (it == std::string::npos) return false;
  input.erase(it, suffix.length());
  return true;
}

// Strip suffix pattern like ".llvm.283491234". The given suffix only
// contains ".llvm." so we need to find the appendix "283491234" after ".llvm."
// in the input string and strip it together with ".llvm.".
static bool StripSuffixWithTwoDots(std::string &input,
                                   const std::string &suffix) {
  auto it = input.rfind(suffix);
  if (it == std::string::npos) return false;
  // find the "." belonging to the next suffix after the current suffix
  // like ".llvm."
  auto dot_after_suffix_pos = input.find_first_of('.', it + suffix.length());
  // strip everything in the range of [it, dot_after_suffix_pos)
  input.erase(it, dot_after_suffix_pos == std::string::npos
                      ? std::string::npos
                      : (dot_after_suffix_pos - it));
  return true;
}

std::string SymbolMap::GetOriginalName(const char *name) const {
  if (suffix_elision_policy_ == ElideNone) {
    return name;
  } else if (suffix_elision_policy_ == ElideAll) {
    const char *split = strchr(name, '.');

    // FIXME: We currently represent symbols in our profiles that have no
    // associated debug information. Some of these symbols are defined in
    // assembly, and begin with '.' (thanks, `nasm`). Splitting on the first '.'
    // therefore gives us an empty symbol name, which turns into a parse error
    // if used as an indirect call target in a text profile.
    if (split && split != name) {
      return std::string(name, split - name);
    } else {
      return std::string(name);
    }
  } else if (suffix_elision_policy_ == ElideSelected) {
    std::string cand(name);
    for (std::string suffix : selectedSuffixes) {
      if (suffix == ".cold")
        StripLastOccurrenceOf(cand, suffix);
      else if (suffix == ".llvm.")
        StripSuffixWithTwoDots(cand, suffix);
      else if (suffix == ".lto_priv.")
        StripSuffixWithTwoDots(cand, suffix);
      else if (suffix == ".part.")
        StripSuffixWithTwoDots(cand, suffix);
      else if (suffix == ".isra.")
        StripSuffixWithTwoDots(cand, suffix);
    }
    return cand;
  } else {
    LOG(FATAL) << "unknown suffix elision policy";
    return name;
  }
}

void SymbolMap::ElideSuffixesAndMerge() {
  std::vector<std::string> suffix_elide_set;
  for (auto &name_symbol : map_) {
    std::string orig_name = GetOriginalName(name_symbol.first.c_str());
    if (orig_name != name_symbol.first.c_str())
      suffix_elide_set.push_back(name_symbol.first.c_str());
  }
  for (const auto &name : suffix_elide_set) {
    std::string orig_name = GetOriginalName(name.c_str());
    auto iter = map_.find(name);
    CHECK(iter != map_.end());
    Symbol *sym = iter->second;
    map_.erase(iter);

    std::pair<NameSymbolMap::iterator, bool> ret =
        map_.insert(NameSymbolMap::value_type(orig_name, NULL));
    if (ret.second || sym == ret.first->second) {
      unique_symbols_.push_back(
          absl::make_unique<Symbol>(ret.first->first.c_str(), "", "", 0));
      ret.first->second = unique_symbols_.back().get();
    }

    ret.first->second->Merge(sym);
    for (auto &n_s : map_) {
      if (n_s.second == sym)
        n_s.second = ret.first->second;
    }
  }
}

void SymbolMap::AddSymbol(const std::string &name) {
  std::pair<NameSymbolMap::iterator, bool> ret = map_.insert(
      NameSymbolMap::value_type(name, NULL));
  if (ret.second) {
    unique_symbols_.push_back(
        absl::make_unique<Symbol>(ret.first->first.c_str(), "", "", 0));
    ret.first->second = unique_symbols_.back().get();
    NameAliasMap::const_iterator alias_iter = name_alias_map_.find(name);
    if (alias_iter != name_alias_map_.end()) {
      for (const auto &name : alias_iter->second) {
        map_[name] = ret.first->second;
      }
    }
  }
}

void SymbolMap::AddSymbolMappings(const NameSymbolMap &new_map) {
  absl::flat_hash_set<Symbol *> new_symbols;
  for (const auto &name_symbol : new_map) {
    auto ret = new_symbols.insert(name_symbol.second);
    if (ret.second) {
      unique_symbols_.push_back(absl::WrapUnique(name_symbol.second));
    }
    map_[name_symbol.first] = name_symbol.second;
  }
}

void SymbolMap::CalculateThresholdFromTotalCount(int64_t total_count) {
  count_threshold_ = total_count * absl::GetFlag(FLAGS_sample_threshold_frac);
  if (count_threshold_ < kMinSamples) {
    count_threshold_ = kMinSamples;
  }
}

void SymbolMap::CalculateThreshold() {
  // If count_threshold_ is pre-calculated, use pre-caculated value.
  CHECK_EQ(count_threshold_, 0);
  int64_t total_count = 0;
  absl::flat_hash_set<std::string> visited;
  for (const auto &name_symbol : map_) {
    if (visited.insert(name_symbol.second->name()).second) {
      total_count += name_symbol.second->total_count;
    }
  }
  count_threshold_ = total_count * absl::GetFlag(FLAGS_sample_threshold_frac);
  if (count_threshold_ < kMinSamples) {
    count_threshold_ = kMinSamples;
  }
}

const bool SymbolMap::GetSymbolInfoByAddr(uint64_t addr,
                                          const std::string **name,
                                          uint64_t *start_addr,
                                          uint64_t *end_addr) const {
  AddressSymbolMap::const_iterator ret = address_symbol_map_.upper_bound(addr);
  if (ret == address_symbol_map_.begin()) {
    return false;
  }
  ret--;
  if (addr >= ret->first && addr < ret->first + ret->second.second) {
    if (name) {
      *name = &ret->second.first;
    }
    if (start_addr) {
      *start_addr = ret->first;
    }
    if (end_addr) {
      *end_addr = ret->first + ret->second.second;
    }
    return true;
  } else {
    return false;
  }
}

const std::string *SymbolMap::GetSymbolNameByStartAddr(uint64_t addr) const {
  AddressSymbolMap::const_iterator ret = address_symbol_map_.find(addr);
  if (ret == address_symbol_map_.end()) {
    return NULL;
  }
  return &ret->second.first;
}

class SymbolReader : public ElfReader::SymbolSink {
 public:
  explicit SymbolReader(NameAliasMap *name_alias_map,
                        AddressSymbolMap *address_symbol_map)
      : name_alias_map_(name_alias_map),
        address_symbol_map_(address_symbol_map) { }
  void AddSymbol(const char *name, uint64_t address, uint64_t size, int binding,
                 int type, int section) override {
    if (strcmp(name, SymbolMap::get_fs_discriminator_symbol()) == 0)
      use_fs_discriminaor_ = true;
    std::pair<AddressSymbolMap::iterator, bool> ret =
        address_symbol_map_->insert(
            std::make_pair(address, std::make_pair(std::string(name), size)));
    if (!ret.second) {
      (*name_alias_map_)[ret.first->second.first].insert(name);
    }
  }
  virtual ~SymbolReader() { }
  bool use_fs_discriminaor() const { return use_fs_discriminaor_; }

 private:
  NameAliasMap *name_alias_map_;
  AddressSymbolMap *address_symbol_map_;
  bool use_fs_discriminaor_ = false;

  DISALLOW_COPY_AND_ASSIGN(SymbolReader);
};


void SymbolMap::BuildSymbolMap() {
  ElfReader elf_reader(binary_);
  base_addr_ = elf_reader.VaddrOfFirstLoadSegment();
  SymbolReader symbol_reader(&name_alias_map_, &address_symbol_map_);
  symbol_reader.filter = [](const char *name, uint64 address, uint64 size,
                            int binding, int type, int section) {
    if (strcmp(name, get_fs_discriminator_symbol()) == 0) return true;
    return (size != 0 && (type == STT_FUNC || absl::EndsWith(name, ".cold")) &&
            strcmp(name + strlen(name) - 4, "@plt") != 0);
  };
  elf_reader.VisitSymbols(&symbol_reader);
#if defined(HAVE_LLVM)
  if (symbol_reader.use_fs_discriminaor() ||
      absl::GetFlag(FLAGS_use_fs_discriminator))
    SourceInfo::use_fs_discriminator = true;
#endif
}

void SymbolMap::UpdateSymbolMap(
    const Addr2line *addr2line,
    const std::map<uint64_t, uint64_t> &sampled_functions) {
  for (const auto &addr_size : sampled_functions) {
    std::string name = address_symbol_map_.find(addr_size.first)->second.first;
    SourceStack stack;
    addr2line->GetInlineStack(addr_size.first, &stack);
    if (!stack.empty()) {
      // Map from symbol name to "Symbol *".
      auto ret = map_.insert(std::make_pair(name, nullptr));
      if (ret.second) {
        unique_symbols_.push_back(absl::make_unique<Symbol>());
        ret.first->second = unique_symbols_.back().get();
      }
      ret.first->second->info = stack[stack.size() - 1];
    }
  }
}

std::string Symbol::ModuleName() const {
  // This is a special case in Google3, though tcmalloc.cc has a suffix of .cc,
  // it's actually no a module, but included by tcmalloc_or_debug.cc, which is
  // a pure wrapper. Thus when a function is found to belong to module
  // tcmalloc.cc, it should be reattributed to the wrapper module.
  if (info.RelativePath() == "./tcmalloc/tcmalloc.cc") {
    return "tcmalloc/tcmalloc_or_debug.cc";
  } else {
    return info.RelativePath();
  }
}

void SymbolMap::AddSymbolEntryCount(const std::string &symbol_name,
                                    uint64_t head_count, uint64_t total_count) {
  Symbol *symbol = map_.find(symbol_name)->second;
  symbol->head_count += head_count;
  symbol->total_count += total_count;
}

Symbol *SymbolMap::TraverseInlineStack(const std::string &symbol_name,
                                       const SourceStack &src, uint64_t count,
                                       DataSource data_source) {
  if (src.empty()) return nullptr;
  bool use_discriminator_encoding =
      absl::GetFlag(FLAGS_use_discriminator_encoding);
  Symbol *symbol = map_.find(symbol_name)->second;
  symbol->total_count += count;
  const SourceInfo &info = src[src.size() - 1];
  if (symbol->info.file_name.empty() && !info.file_name.empty()) {
    symbol->info.file_name = info.file_name;
    symbol->info.dir_name = info.dir_name;
  }
  for (int i = src.size() - 1; i > 0; i--) {
    if ((data_source == PERFDATA || data_source == AFDOPROTO) &&
        src[i].HasInvalidInfo())
      break;
    std::pair<CallsiteMap::iterator, bool> ret =
        symbol->callsites.insert(CallsiteMap::value_type(
            Callsite(src[i].Offset(use_discriminator_encoding),
                     src[i - 1].func_name),
            NULL));
    if (ret.second) {
      ret.first->second = new Symbol(src[i - 1].func_name,
                                     src[i - 1].dir_name,
                                     src[i - 1].file_name,
                                     src[i - 1].start_line);
    }
    symbol = ret.first->second;
    symbol->total_count += count;
  }
  return symbol;
}

void SymbolMap::AddSourceCount(const std::string &symbol_name,
                               const SourceStack &src, uint64_t count,
                               uint64_t num_inst, uint32_t duplication,
                               DataSource data_source) {
  bool use_discriminator_encoding =
      absl::GetFlag(FLAGS_use_discriminator_encoding);
  if (duplication != 1 &&
      absl::GetFlag(FLAGS_use_discriminator_multiply_factor))
    count *= duplication;
  Symbol *symbol = TraverseInlineStack(symbol_name, src, count, data_source);
  if (!symbol) return;
  bool need_conversion = (data_source == PERFDATA || data_source == AFDOPROTO);
  if (need_conversion && src[0].HasInvalidInfo()) return;
  uint64_t offset = src[0].Offset(use_discriminator_encoding);
  // If it is to convert perf data or afdoproto to afdo profile, select the
  // MAX count if there are multiple records mapping to the same offset.
  // If it is just to read afdo profile, merge those counts.
  if (need_conversion) {
    if (count > symbol->pos_counts[offset].count) {
      symbol->pos_counts[offset].count = count;
    }
  } else {
    symbol->pos_counts[offset].count += count;
  }
  symbol->pos_counts[offset].num_inst += num_inst;
}

bool SymbolMap::AddIndirectCallTarget(const std::string &symbol_name,
                                      const SourceStack &src,
                                      const std::string &target, uint64_t count,
                                      DataSource data_source) {
  bool use_discriminator_encoding =
      absl::GetFlag(FLAGS_use_discriminator_encoding);
  Symbol *symbol = TraverseInlineStack(symbol_name, src, 0, data_source);
  if (!symbol) return false;
  if ((data_source == PERFDATA || data_source == AFDOPROTO) &&
      src[0].HasInvalidInfo())
    return false;
  symbol->pos_counts[src[0].Offset(use_discriminator_encoding)]
      .target_map[GetOriginalName(target.c_str())] = count;
  return true;
}

// Compute total_count_incl of current symbol and total_count_incls of all the
// inline instances.
void Symbol::ComputeTotalCountIncl(const NameSymbolMap &nsmap,
                                   std::vector<Symbol *> *stacksyms,
                                   absl::flat_hash_set<Symbol *> *scc) {
  for (const auto &pos_count : pos_counts) {
    for (const auto &target_count : pos_count.second.target_map) {
      auto iter = nsmap.find(target_count.first);
      if (iter == nsmap.end()) continue;
      Symbol *callee = iter->second;
      if (scc->count(callee)) continue;
      uint64_t calltimes = callee->head_count ? callee->head_count : 1;
      // callee_time is the time spent on calling this callee and all its
      // decendents.
      uint64_t callee_time =
          static_cast<uint64_t>(static_cast<float>(callee->total_count_incl) /
                                calltimes * target_count.second);
      for (auto parent_sym : *stacksyms)
        parent_sym->total_count_incl += callee_time;
    }
  }

  std::vector<Callsite> calls;
  for (const auto &pair : callsites) {
    Symbol *inline_instance = pair.second;
    inline_instance->total_count_incl = inline_instance->total_count;
    stacksyms->push_back(inline_instance);
    inline_instance->ComputeTotalCountIncl(nsmap, stacksyms, scc);
    stacksyms->pop_back();
  }
}

void Symbol::DumpBody(int ident, bool for_analysis) const {
  std::vector<uint64_t> positions;
  for (const auto &pos_count : pos_counts)
    positions.push_back(pos_count.first);
  std::sort(positions.begin(), positions.end());
  for (const auto &pos : positions) {
    PositionCountMap::const_iterator ret = pos_counts.find(pos);
    DCHECK(ret != pos_counts.end());
    PrintSourceLocation(info.start_line, pos, ident + 2);
    absl::PrintF("%u", ret->second.count);
    TargetCountPairs target_count_pairs;
    GetSortedTargetCountPairs(ret->second.target_map,
                              &target_count_pairs);
    for (const auto &target_count : target_count_pairs) {
      const std::string printed_name = getPrintName(target_count.first.data());
      absl::PrintF("  %s:%u", printed_name, target_count.second);
    }
    printf("\n");
  }
  std::vector<Callsite> calls;
  for (const auto &pos_symbol : callsites) {
    calls.push_back(pos_symbol.first);
  }
  std::sort(calls.begin(), calls.end(), CallsiteLessThan());
  for (const auto &callsite : calls) {
    PrintSourceLocation(info.start_line, callsite.first, ident + 2);
    if (for_analysis)
      callsites.find(callsite)->second->DumpForAnalysis(ident + 2);
    else
      callsites.find(callsite)->second->Dump(ident + 2);
  }
}

void Symbol::Dump(int ident) const {
  const std::string printed_name = getPrintName(info.func_name);
  if (ident == 0) {
    absl::PrintF("%s total:%u head:%u\n", printed_name, total_count,
                 head_count);
  } else {
    absl::PrintF("%s total:%u\n", printed_name, total_count);
  }
  DumpBody(ident, false);
}

void Symbol::DumpForAnalysis(int ident) const {
  const std::string printed_name = getPrintName(info.func_name);
  if (ident == 0) {
    absl::PrintF(
        "%s total:%u head:%u total_incl:%u total_incl_per_iter:%.2f\n",
        printed_name, total_count, head_count, total_count_incl,
        head_count ? static_cast<float>(total_count_incl) / head_count : 0);
  } else {
    absl::PrintF("%s total:%u total_incl:%u\n", printed_name, total_count,
                 total_count_incl);
  }
  DumpBody(ident, true);
}

void Symbol::UpdateWithRatio(double ratio) {
  total_count = roundl(total_count * ratio);
  head_count = roundl(head_count * ratio);
  for (auto &pos_count : pos_counts) {
    pos_count.second.count = roundl(pos_count.second.count * ratio);
    auto &target_map = pos_count.second.target_map;
    for (auto &target_count : target_map) {
      target_count.second = roundl(target_count.second * ratio);
    }
  }
  for (const auto &callsite : callsites) {
    callsite.second->UpdateWithRatio(ratio);
  }
}

// Returns the count of either pos_counts or callsites that has the lowest
// offset. Searches one with lowest offset as pos_counts and callsites are not
// ordered.
uint64_t Symbol::EntryCount() const {
  // Ignores very large position offsets because it suggests that instructions
  // got moved and may misrepresent the hotnesss of the original code path.
  const uint32_t kMaxValidLine = 10000;
  SourceInfo max_source;
  max_source.line = kMaxValidLine;

  uint64_t entry_count = 0;
  uint64_t min_pos = max_source.Offset(false);
  for (const auto& pos_count : pos_counts) {
    if (pos_count.first < min_pos) {
      min_pos = pos_count.first;
      entry_count = pos_count.second.count;
    }
  }
  for (const auto& callsite : callsites) {
    if (callsite.first.first < min_pos) {
      min_pos = callsite.first.first;
      entry_count = callsite.second->EntryCount();
    }
  }
  return entry_count;
}

struct SCCNode {
  union {
    unsigned index = 0;
    unsigned pre_visited;
  };
  union {
    unsigned lowlink = 0;
    unsigned post_visited;
  };
  absl::flat_hash_set<Symbol *> syms;
  absl::flat_hash_set<SCCNode *> callees;
  static void InsertCallEdge(SCCNode *caller, SCCNode *callee) {
    caller->callees.insert(callee);
  }
  void Dump() { LOG(INFO) << *this; }
  friend std::ostream &operator<<(std::ostream &os, const SCCNode &node);
};

std::ostream &operator<<(std::ostream &os, const SCCNode &node) {
  os << "node<";
  for (auto sym : node.syms) {
    if (sym == *(node.syms.begin()))
      os << sym->name();
    else
      os << ", " << sym->name();
  }
  os << ">";
  return os;
}

class CallGraph {
 public:
  CallGraph() = default;
  ~CallGraph() {
    for (auto node : nodes) delete node;
    for (auto node : delete_nodes) delete node;
  }
  using NodeStack = std::vector<SCCNode *>;
  using SCCMap = absl::flat_hash_map<SCCNode *, SCCNode *>;
  void ReverseTopoSort(std::vector<SCCNode *> *sorted);
  SCCNode *FindOrCreateSCCNode(Symbol *sym);
  void FindAndCollapseSCC();
  void Dump();

 private:
  void TopoVisit(SCCNode *node, absl::flat_hash_set<SCCNode *> *workingset,
                 std::vector<SCCNode *> *sorted);
  SCCNode *CreateSCCNode(Symbol *sym);
  void StrongConnect(SCCNode *node, unsigned index, NodeStack *stack,
                     SCCMap *sccmap);
  void CollapseSCC(SCCMap *sccmap);

  absl::flat_hash_set<SCCNode *> nodes;
  absl::flat_hash_set<SCCNode *> delete_nodes;
  absl::flat_hash_map<Symbol *, SCCNode *> sym_scc_map;
  DISALLOW_COPY_AND_ASSIGN(CallGraph);
};

// Visit all the nodes using DFS and return in reverse-topological order.
void CallGraph::TopoVisit(SCCNode *node,
                          absl::flat_hash_set<SCCNode *> *workingset,
                          std::vector<SCCNode *> *sorted) {
  if (node->post_visited) return;
  if (node->pre_visited) LOG(FATAL) << "Found a Loop";
  node->pre_visited = 1;
  for (auto callee : node->callees) TopoVisit(callee, workingset, sorted);
  node->post_visited = 1;
  workingset->erase(node);
  sorted->push_back(node);
}

// Reverse-topologically sort all the nodes in the CallGraph, and save
// the result in the vector sorted.
void CallGraph::ReverseTopoSort(std::vector<SCCNode *> *sorted) {
  std::for_each(nodes.begin(), nodes.end(), [](SCCNode *node) {
    node->pre_visited = 0;
    node->post_visited = 0;
  });
  absl::flat_hash_set<SCCNode *> workingset = nodes;
  while (!workingset.empty()) {
    TopoVisit(*workingset.begin(), &workingset, sorted);
  }
}

SCCNode *CallGraph::CreateSCCNode(Symbol *sym) {
  SCCNode *new_node = new SCCNode();
  new_node->syms.insert(sym);
  nodes.insert(new_node);
  sym_scc_map.insert({sym, new_node});
  return new_node;
}

SCCNode *CallGraph::FindOrCreateSCCNode(Symbol *sym) {
  auto it = sym_scc_map.find(sym);
  if (it != sym_scc_map.end()) return it->second;
  return CreateSCCNode(sym);
}

// Find the strongly connected component using DFS. sccmap is the map mapping
// each node to the root node of the SCC where node belongs to.
void CallGraph::StrongConnect(SCCNode *node, unsigned index, NodeStack *stack,
                              SCCMap *sccmap) {
  node->index = index;
  node->lowlink = index;
  index++;
  stack->push_back(node);

  for (auto callee : node->callees) {
    if (!callee->index) {
      StrongConnect(callee, index, stack, sccmap);
      node->lowlink = std::min(node->lowlink, callee->lowlink);
    } else if (std::find(stack->begin(), stack->end(), callee) !=
               stack->end()) {
      node->lowlink = std::min(node->lowlink, callee->lowlink);
    }
  }

  if (node->index == node->lowlink) {
    SCCNode *member = nullptr;
    do {
      member = stack->back();
      stack->pop_back();
      sccmap->insert({member, node});
    } while (member != node);
  }
}

// Collapse the strongly connected nodes into one SCC node. Before CollapseSCC,
// each SCCNode only contains one symbol. After CollapseSCC, each SCCNode
// contains all the symbols belong to the same SCC.
void CallGraph::CollapseSCC(SCCMap *sccmap) {
  for (auto node : nodes) {
    auto sccroot = (*sccmap)[node];
    if (node != sccroot) {
      sccroot->syms.insert(node->syms.begin(), node->syms.end());
      for (auto callee : node->callees) {
        if ((*sccmap)[callee] != sccroot)
          sccroot->callees.insert((*sccmap)[callee]);
      }
      delete_nodes.insert(node);
    } else {
      absl::flat_hash_set<SCCNode *> to_erase;
      for (auto callee : node->callees) {
        if (callee != (*sccmap)[callee]) to_erase.insert(callee);
      }
      for (auto erase_node : to_erase) {
        node->callees.erase(erase_node);
        node->callees.insert((*sccmap)[erase_node]);
      }
      node->callees.erase(node);
    }
  }
  for (auto pair : sym_scc_map) pair.second = (*sccmap)[pair.second];
  for (auto node : delete_nodes) nodes.erase(node);
}

void CallGraph::FindAndCollapseSCC() {
  unsigned index = 1;
  NodeStack stack;
  SCCMap sccmap;
  for (auto node : nodes) {
    if (!node->index) StrongConnect(node, index, &stack, &sccmap);
  }
  CollapseSCC(&sccmap);
}

void CallGraph::Dump() {
  LOG(INFO) << "====== Dump CallGraph: ======";
  for (auto node : nodes) {
    LOG(INFO) << *node << " calls";
    for (auto callee : node->callees) {
      LOG(INFO) << "  " << *callee;
    }
    LOG(INFO) << "\n";
  }
}

void Symbol::BuildCallGraph(const NameSymbolMap &nsmap, SCCNode *callerSCC,
                            CallGraph *callgraph) {
  for (const auto &pos_count : pos_counts) {
    for (const auto &target_count : pos_count.second.target_map) {
      auto iter = nsmap.find(target_count.first);
      if (iter == nsmap.end()) continue;
      Symbol *callee = iter->second;
      SCCNode *calleeSCC = callgraph->FindOrCreateSCCNode(callee);
      SCCNode::InsertCallEdge(callerSCC, calleeSCC);
    }
  }
  for (const auto &pair : callsites) {
    Symbol *inline_instance = pair.second;
    inline_instance->BuildCallGraph(nsmap, callerSCC, callgraph);
  }
}

void SymbolMap::BuildCallGraph(CallGraph *callgraph) {
  for (const auto &pair : map_) {
    Symbol *sym = pair.second;
    SCCNode *callerSCC = callgraph->FindOrCreateSCCNode(sym);
    sym->BuildCallGraph(map_, callerSCC, callgraph);
  }
}

// Compute total_count_incl of all the function symbols in the symbol map.
// Unlike total_count, total_count_incl includes the sample count of all
// decendents called by the function symbol. It represents the accumulated
// sample counts on the way from entering the function to exiting the function.
// symbols have to be computed in the reverse topological order of callgraph.
void SymbolMap::ComputeTotalCountIncl() {
  CallGraph callgraph;
  std::vector<SCCNode *> sorted;

  // Build callgraph, collapse cycles into SCC node and get the reverse
  // topological order of all the SCC nodes.
  BuildCallGraph(&callgraph);
  callgraph.FindAndCollapseSCC();
  callgraph.ReverseTopoSort(&sorted);

  // Compute the total_count_incl. Every symbol in the same SCC node has the
  // same total_count_incl.
  std::vector<Symbol *> stacksyms;
  for (auto node : sorted) {
    unsigned scc_total_count_incl = 0;
    for (auto sym : node->syms) {
      sym->total_count_incl = sym->total_count;
      stacksyms.push_back(sym);
      sym->ComputeTotalCountIncl(map_, &stacksyms, &node->syms);
      stacksyms.pop_back();
      scc_total_count_incl += sym->total_count_incl;
    }
    for (auto sym : node->syms) sym->total_count_incl = scc_total_count_incl;
  }
}

void SymbolMap::Dump(bool dump_for_analysis) const {
  std::map<uint64_t, std::set<std::string> > count_names_map;
  for (const auto &name_symbol : map_) {
    if (name_symbol.second->total_count > 0) {
      count_names_map[~name_symbol.second->total_count].insert(
          name_symbol.first);
    }
  }
  for (const auto &count_names : count_names_map) {
    for (const auto &name : count_names.second) {
      Symbol *symbol = map_.find(name)->second;
      if (dump_for_analysis)
        symbol->DumpForAnalysis(0);
      else
        symbol->Dump(0);
    }
  }
}

float SymbolMap::Overlap(const SymbolMap &map) const {
  std::map<std::string, std::pair<uint64_t, uint64_t> > overlap_map;

  // Prepare for overlap_map
  uint64_t total_1 = 0;
  uint64_t total_2 = 0;
  for (const auto &name_symbol : map_) {
    total_1 += name_symbol.second->total_count;
    overlap_map[name_symbol.first].first = name_symbol.second->total_count;
    overlap_map[name_symbol.first].second = 0;
  }
  for (const auto &name_symbol : map.map()) {
    if (overlap_map.find(name_symbol.first) == overlap_map.end()) {
      overlap_map[name_symbol.first].first = 0;
    }
    total_2 += name_symbol.second->total_count;
    overlap_map[name_symbol.first].second = name_symbol.second->total_count;
  }

  if (total_1 == 0 || total_2 == 0) {
    return 0.0;
  }

  // Calculate the overlap
  float overlap = 0.0;
  for (const auto &name_counts : overlap_map) {
    overlap += std::min(
        static_cast<float>(name_counts.second.first) / total_1,
        static_cast<float>(name_counts.second.second) / total_2);
  }
  return overlap;
}

void SymbolMap::DumpFuncLevelProfileCompare(const SymbolMap &map) const {
  uint64_t max_1 = 0;
  uint64_t max_2 = 0;

  // Calculate the max of the two maps
  for (const auto &name_symbol : map_) {
    max_1 = std::max(name_symbol.second->total_count, max_1);
  }
  for (const auto &name_symbol : map.map()) {
    max_2 = std::max(name_symbol.second->total_count, max_2);
  }

  // Sort map_1
  std::map<uint64_t, std::vector<std::string> > count_names_map;
  for (const auto &name_symbol : map_) {
    if (name_symbol.second->total_count > 0) {
      count_names_map[name_symbol.second->total_count].push_back(
          name_symbol.first);
    }
  }
  // Dump hot functions in map_1
  for (auto count_names_iter = count_names_map.rbegin();
       count_names_iter != count_names_map.rend(); ++count_names_iter) {
    for (const auto &name : count_names_iter->second) {
      Symbol *symbol = map_.find(name)->second;
      if (symbol->total_count * 100 <
          max_1 * absl::GetFlag(FLAGS_dump_cutoff_percent)) {
        break;
      }

      const auto &iter = map.map().find(name);
      uint64_t compare_count = 0;
      if (iter != map.map().end()) {
        compare_count = iter->second->total_count;
      }
      printf("%3.4f%% %3.4f%% %s\n",
             100 * static_cast<double>(symbol->total_count) / max_1,
             100 * static_cast<double>(compare_count) / max_2,
             getPrintName(name.c_str()).c_str());
    }
  }

  // Sort map_2
  count_names_map.clear();
  for (const auto &name_symbol : map.map()) {
    if (name_symbol.second->total_count > 0) {
      count_names_map[name_symbol.second->total_count].push_back(
          name_symbol.first);
    }
  }
  // Dump hot functions in map_2 that was not caught.
  for (auto count_names_iter = count_names_map.rbegin();
       count_names_iter != count_names_map.rend(); ++count_names_iter) {
    for (const auto &name : count_names_iter->second) {
      Symbol *symbol = map.map().find(name)->second;
      if (symbol->total_count * 100 <
          max_2 * absl::GetFlag(FLAGS_dump_cutoff_percent)) {
        break;
      }

      const auto &iter = map_.find(name);
      uint64_t compare_count = 0;
      if (iter != map_.end()) {
        compare_count = iter->second->total_count;
        if (compare_count * 100 >=
            max_1 * absl::GetFlag(FLAGS_dump_cutoff_percent)) {
          continue;
        }
      }
      printf("%3.4f%% %3.4f%% %s\n",
             100 * static_cast<double>(compare_count) / max_1,
             100 * static_cast<double>(symbol->total_count) / max_2,
             getPrintName(name.c_str()).c_str());
    }
  }
}

typedef std::map<uint64_t, uint64_t> Histogram;

static uint64_t AddSymbolProfileToHistogram(const Symbol *symbol,
                                            Histogram *histogram) {
  uint64_t total_count = 0;
  for (const auto &pos_count : symbol->pos_counts) {
    std::pair<Histogram::iterator, bool> ret =
        histogram->insert(Histogram::value_type(pos_count.second.count, 0));
    ret.first->second += pos_count.second.num_inst;
    total_count += pos_count.second.count * pos_count.second.num_inst;
  }
  for (const auto &callsite_symbol : symbol->callsites) {
    total_count += AddSymbolProfileToHistogram(callsite_symbol.second,
                                               histogram);
  }
  return total_count;
}

void SymbolMap::ComputeWorkingSets() {
  Histogram histogram;
  uint64_t total_count = 0;

  // Step 1. Compute histogram.
  for (const auto &symbol : unique_symbols_) {
    if (symbol->total_count == 0) {
      continue;
    }
    total_count += AddSymbolProfileToHistogram(symbol.get(), &histogram);
  }
  int bucket_num = 0;
  uint64_t accumulated_count = 0;
  uint64_t accumulated_inst = 0;
  uint64_t one_bucket_count = total_count / (NUM_GCOV_WORKING_SETS + 1);

  // Step 2. Traverse the histogram to update the working set.
  for (Histogram::const_reverse_iterator iter = histogram.rbegin();
       iter != histogram.rend() && bucket_num < NUM_GCOV_WORKING_SETS; ++iter) {
    uint64_t count = iter->first;
    uint64_t num_inst = iter->second;
    while (count * num_inst + accumulated_count
           > one_bucket_count * (bucket_num + 1)
           && bucket_num < NUM_GCOV_WORKING_SETS) {
      int64_t offset =
          (one_bucket_count * (bucket_num + 1) - accumulated_count) / count;
      accumulated_inst += offset;
      accumulated_count += offset * count;
      num_inst -= offset;
      working_set_[bucket_num].num_counters = accumulated_inst;
      working_set_[bucket_num].min_counter = count;
      bucket_num++;
    }
    accumulated_inst += num_inst;
    accumulated_count += num_inst * count;
  }
}

std::map<uint64_t, uint64_t> SymbolMap::GetSampledSymbolStartAddressSizeMap(
    const std::set<uint64_t> &sampled_addrs) const {
  // We depend on the fact that sampled_addrs is an ordered set.
  std::map<uint64_t, uint64_t> ret;
  uint64_t next_start_addr = 0;
  for (const auto &addr : sampled_addrs) {
    uint64_t adjusted_addr = addr + base_addr_;
    if (adjusted_addr < next_start_addr) {
      continue;
    }

    AddressSymbolMap::const_iterator iter =
        address_symbol_map_.upper_bound(adjusted_addr);
    if (iter == address_symbol_map_.begin()) {
      continue;
    }
    iter--;
    ret.insert(std::make_pair(iter->first, iter->second.second));
    next_start_addr = iter->first + iter->second.second;
  }
  for (const auto &addr_symbol : address_symbol_map_) {
    if (ret.find(addr_symbol.first) != ret.end()) {
      continue;
    }
    const auto &iter = map_.find(addr_symbol.second.first);
    if (iter != map_.end() && iter->second != NULL
        && iter->second->total_count > 0) {
      ret[addr_symbol.first] = addr_symbol.second.second;
    }
  }
  return ret;
}

// SymbolMap has already been read from the old profile. This function traverses
// symbol map to calculate the functions that have samples.
std::map<uint64_t, uint64_t> SymbolMap::GetLegacySymbolStartAddressSizeMap()
    const {
  std::set<std::string> names;
  // Traverse all top level symbols, including all inlined symbols. If the
  // symbol's total count is non-zero, it has samples and should be included
  // in the return value.
  for (const auto &s : unique_symbols_) {
    if (s->total_count == 0) {
      continue;
    }
    std::vector<const Symbol *> queue;
    queue.push_back(s.get());
    while (!queue.empty()) {
      const Symbol *s = queue.back();
      queue.pop_back();
      if (s->total_count == 0) {
        continue;
      }
      names.insert(s->info.func_name);
      for (const auto &pos_symbol : s->callsites) {
        queue.push_back(pos_symbol.second);
      }
    }
  }
  std::map<uint64_t, uint64_t> ret;
  for (const std::string &name : names) {
    const auto &iter = name_addr_map_.find(name);
    if (iter == name_addr_map_.end()) {
      continue;
    }
    const auto &a_s_iter = address_symbol_map_.find(iter->second);
    CHECK(a_s_iter != address_symbol_map_.end());
    ret[a_s_iter->first] = a_s_iter->second.second;
  }
  return ret;
}

void SymbolMap::AddAlias(const std::string &sym, const std::string &alias) {
  name_alias_map_[sym].insert(alias);
}

// Consts for profile validation
// The min thresholds here are intentionally set to very low values for
// only coarse grain validation. They are only used to prevent very bad
// things from happening. For fine grain profile quality control, gwp
// autofdo pipeline has much more variables to check. The thresholds for
// those variables in gwp side are usually set to high values and gwp
// side can customize those thresholds for individual project.
static const int kMinNumSymbols = 10;
static const int kMinTotalCount = 1000000;
static const float kMinNonZeroSrcFrac = 0.6;

bool SymbolMap::Validate() const {
  if (size() < kMinNumSymbols) {
    LOG(ERROR) << "# of symbols (" << size() << ") too small.";
    return false;
  }
  uint64_t total_count = 0;
  uint64_t num_srcs = 0;
  uint64_t num_srcs_non_zero = 0;
  bool has_inline_stack = false;
  bool has_call = false;
  bool has_discriminator = false;
  std::vector<const Symbol *> symbols;
  for (const auto &s : unique_symbols_) {
    if (s->total_count == 0) {
      continue;
    }
    total_count += s->total_count;
    symbols.push_back(s.get());
    if (!s->callsites.empty()) {
      has_inline_stack = true;
    }
  }
  while (!symbols.empty()) {
    const Symbol *symbol = symbols.back();
    symbols.pop_back();
    for (const auto &pos_count : symbol->pos_counts) {
      if (pos_count.second.target_map.size() > 0) {
        has_call = true;
      }
      num_srcs++;
      if (pos_count.first != 0) {
        num_srcs_non_zero++;
      }
      if (SourceInfo::GetDiscriminatorFromOffset(pos_count.first) != 0) {
        has_discriminator = true;
      }
    }
    for (const auto &pos_callsite : symbol->callsites) {
      symbols.push_back(pos_callsite.second);
    }
  }
  if (total_count < kMinTotalCount) {
    LOG(ERROR) << "Total count (" << total_count << ") too small.";
    return false;
  }
  if (!has_call) {
    LOG(ERROR) << "Do not have a single call.";
    return false;
  }
  if (!has_inline_stack) {
    LOG(ERROR) << "Do not have a single inline stack.";
    return false;
  }
  if (!has_discriminator) {
    LOG(ERROR) << "Do not have a single discriminator.";
    return false;
  }
  if (num_srcs_non_zero < num_srcs * kMinNonZeroSrcFrac) {
    LOG(ERROR) << "Do not have enough non-zero src locations."
               << " NonZero: " << num_srcs_non_zero
               << " Total: " << num_srcs;
    return false;
  }
  return true;
}

uint64_t SymbolMap::GetTotalSum() {
  uint64_t totalsum = 0;
  for (const auto &name_symbol : map_)
    totalsum += name_symbol.second->total_count;
  return totalsum;
}

void SymbolMap::UpdateWithRatio(double ratio) {
  for (const auto &name_symbol : map_) {
    name_symbol.second->UpdateWithRatio(ratio);
  }
}

// This function traverses the callsites in the 'other' symbol and...
// * IF the callsite is cold (i.e. total count <= threshold), it flattens the
// callsite in the 'other' symbol, gets/creates an outline symbol and merges
// samples from the callsite into this outline symbol.
// * ELSE copies the callsite (without any flattening or merging) into dest
// symbol.
//
// This function is called on the dest symbol/callsite recursively.
// Once the copying, flattening and merging of the callees is done, it
// copies over the pos_counts map that contains the flattened callsites from
// 'other' to 'this'.
void Symbol::PopulateSymbolRetainingHotInlineStacks(Symbol &other,
                                                    const uint64_t threshold,
                                                    SymbolMap &symMap,
                                                    uint64_t &total,
                                                    uint64_t &num_flattened) {
  total_count += other.total_count;
  head_count += other.head_count;
  if (info.file_name.empty()) {
    info.file_name = other.info.file_name;
    info.dir_name = other.info.dir_name;
  }
  for (const auto &callsite_symbol : other.callsites) {
    ++total;
    // Merge cold callsites into outline symbol and recurse.
    if (callsite_symbol.second->total_count <= threshold) {
      // Here, flatten this callsite w.r.t src symbol, get/create outline symbol
      // and merge callsite into outline symbol.
      ++num_flattened;
      other.FlattenCallsite(callsite_symbol.first.first,
                             callsite_symbol.second);
      symMap.AddSymbolToMap(*callsite_symbol.second);
      symMap.map()
          .at(callsite_symbol.second->info.func_name)
          ->PopulateSymbolRetainingHotInlineStacks(
              *callsite_symbol.second, threshold, symMap, total, num_flattened);
    } else {
      // Copy samples to dest callsite and recurse.
      std::pair<CallsiteMap::iterator, bool> ret = callsites.insert(
          CallsiteMap::value_type(callsite_symbol.first, NULL));
      // If the callsite does not exist in the current symbol, create a new
      // callee symbol with the clone's function name.
      if (ret.second) {
        ret.first->second = new Symbol();
        ret.first->second->info.func_name = ret.first->first.second;
      }
      // This can be a direct call since there is a symbol for this callsite in
      // this dst symbol's callsites map and any changes will be merged into it.
      ret.first->second->PopulateSymbolRetainingHotInlineStacks(
          *callsite_symbol.second, threshold, symMap, total, num_flattened);
    }
  }

  // Do it after the above callsite traversal since that can flatten callsites
  // and those get added into the pos_count. We need to retain those in the dst
  // symbol.
  for (const auto &pos_count : other.pos_counts)
    pos_counts[pos_count.first] += pos_count.second;
}

void SymbolMap::AddSymbolToMap(const Symbol & symbol) {
  // Find or create an entry for current symbol in the flatten map.
  auto map_it = map_.find(symbol.info.func_name);
  if (map_it == map_.end()) {
    AddSymbol(symbol.info.func_name);
  }
}

void SymbolMap::BuildHybridProfile(const SymbolMap &srcmap,
                                   const uint64_t threshold,
                                   uint64_t &num_callsites,
                                   uint64_t &num_flattened) {
  for (const auto &name_symbol : srcmap.map_) {
    AddSymbolToMap(*name_symbol.second);
    map_.at(name_symbol.second->info.func_name)
        ->PopulateSymbolRetainingHotInlineStacks(*name_symbol.second, threshold,
                                                 *this, num_callsites,
                                                 num_flattened);
  }
}

// This function is used to flatten callsites in the srcmap as they
// are copied over into 'this' symbol map. When selectively_flatten is set to
// false, all callsites are flattened (merged into a single outline function).
// When set to true, callsites in cold functions (total count below threshold)
// are converted to direct calls.
void SymbolMap::BuildFlatProfile(const SymbolMap & srcmap,
                                 bool selectively_flatten, uint64_t threshold,
                                 uint64_t &num_total_functions,
                                 uint64_t &num_flattened) {
  std::vector<Symbol *> symbols;
  for (const auto &name_symbol : srcmap.map_) {
    ++num_total_functions;
    if (selectively_flatten && name_symbol.second->total_count >= threshold) {
      AddSymbolToMap(*name_symbol.second);
      map_.at(name_symbol.second->info.func_name)->Merge(name_symbol.second);
    } else {
      ++num_flattened;
      symbols.push_back(name_symbol.second);
    }
  }
  while (!symbols.empty()) {
    Symbol *symbol = symbols.back();
    symbols.pop_back();
    AddSymbolToMap(*symbol);
    for (const auto &pos_callsite : symbol->callsites) {
      pos_callsite.second->EstimateHeadCount();
      symbol->FlattenCallsite(pos_callsite.first.first, pos_callsite.second);
      if (!selectively_flatten) {
        // Add the callsite into current working set.
        symbols.push_back(pos_callsite.second);
      }
    }
    map_.at(symbol->info.func_name)->FlatMerge(symbol);
  }
}

bool SymbolMap::EnsureEntryInFuncForSymbol(const std::string &func_name,
                                           uint64_t pc) {
  if (map().find(func_name) != map().end()) return true;
  AddSymbol(func_name);
  SourceStack stack;
  get_addr2line()->GetInlineStack(pc, &stack);
  // Add bogus samples, so that the writer won't skip over.
  if (!TraverseInlineStack(func_name, stack, count_threshold() + 1)) {
    LOG(WARNING) << "Ignoring address " << std::hex << pc
                 << ". No inline stack found.";
    return false;
  }
  return true;
}

// Removes a symbol by setting total and head count to zero.
void SymbolMap::RemoveSymbol(const std::string &name) {
  for (const auto &name_symbol : map()) {
    if (name == name_symbol.first.c_str()) {
      name_symbol.second->total_count = 0;
      name_symbol.second->head_count = 0;
    }
  }
}

// Removes all the out of line symbols matching the regular expression
// "regex_str" by setting their total and head counts to zero. Those
// symbols with zero counts will be removed when profile is written out.
void SymbolMap::RemoveSymsMatchingRegex(const std::string &regex) {
  for (const auto &name_symbol : map()) {
    if (std::regex_match(name_symbol.first, std::regex(regex))) {
      name_symbol.second->total_count = 0;
      name_symbol.second->head_count = 0;
    }
  }
}

#if defined(HAVE_LLVM)
NameSizeList SymbolMap::collectNamesForProfSymList() {
  llvm::StringSet<> names_in_profile = collectNamesInProfile();
  NameSizeList name_size_list;
  for (const auto &addr_symbol : address_symbol_map_) {
    llvm::StringRef str = addr_symbol.second.first;
    if (names_in_profile.count(str)) continue;
    name_size_list.emplace_back(str, addr_symbol.second.second);
  }
  return name_size_list;
}

llvm::StringSet<> SymbolMap::collectNamesInProfile() {
  std::vector<Symbol *> symbols;
  llvm::StringSet<> names;
  for (const auto &name_symbol : map_) {
    if (name_symbol.second->total_count > 0) {
      symbols.push_back(name_symbol.second);
      // Multiple entries in map_ may share the same Symbol object
      // because of alias. Save the alias name of each entry into
      // names set.
      names.insert(name_symbol.first);
    }
  }

  // Recursively add all the names of outline instances, inline instances
  // and call targets into names set.
  while (!symbols.empty()) {
    Symbol *symbol = symbols.back();
    symbols.pop_back();

    for (const auto &pos_count : symbol->pos_counts) {
      const auto &target_map = pos_count.second.target_map;
      for (const auto &target_count : target_map) {
        names.insert(target_count.first);
      }
    }

    for (const auto &pos_callsite : symbol->callsites) {
      symbols.push_back(pos_callsite.second);
    }
    names.insert(symbol->name());
  }
  return names;
}
#endif

}  // namespace devtools_crosstool_autofdo
