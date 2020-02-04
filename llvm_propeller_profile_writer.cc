#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_propeller_profile_writer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fstream>
#include <functional>
#include <iomanip>
#include <ios>
#include <list>
#include <numeric>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gflags/gflags.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include "third_party/perf_data_converter/src/quipper/perf_parser.h"
#include "third_party/perf_data_converter/src/quipper/perf_reader.h"

DEFINE_string(match_mmap_file, "", "Match mmap event file path.");
DEFINE_bool(ignore_build_id, false, "Ignore build id match.");
DEFINE_bool(gen_path_profile, false, "Generate path profile.");

using llvm::dyn_cast;
using llvm::StringRef;
using llvm::object::ELFFile;
using llvm::object::ELFObjectFile;
using llvm::object::ELFObjectFileBase;
using llvm::object::ObjectFile;
using std::list;
using std::make_pair;
using std::ofstream;
using std::pair;
using std::string;
using std::stringstream;
using std::tuple;

PropellerProfWriter::PropellerProfWriter(const string &bfn, const string &pfn,
                                         const string &ofn)
    : binaryFileName(bfn), perfFileName(pfn), propOutFileName(ofn) {}

PropellerProfWriter::~PropellerProfWriter() {}

// Return true if this SymbolEntry is a containing function for bbName. For
// example, if bbName is given as "aa.BB.foo", and SymbolEntry.name = "foo",
// then SymbolEntry.isFunctionForBBName(bbName) == true. BBNames are from ELF
// object files.
static bool
isFunctionForBBName(SymbolEntry *sym, StringRef bbName) {
  auto a = bbName.split(lld::propeller::BASIC_BLOCK_SEPARATOR);
  if (a.second == sym->name)
    return true;
  for (auto n : sym->aliases)
    if (a.second == n)
      return true;
  return false;
}

static bool
containsAddress(SymbolEntry *sym, uint64_t a) {
  return sym->addr <= a && a < sym->addr + sym->size;
}

static bool
containsAnotherSymbol(SymbolEntry *sym, SymbolEntry *O) {
  if (O->size == 0) {
    // Note if O's size is 0, we allow O on the end boundary. For example, if
    // foo.BB.4 is at address 0x10. foo is [0x0, 0x10), we then assume foo
    // contains foo.BB.4.
    return sym->addr <= O->addr && O->addr <= sym->addr + sym->size;
  }
  return containsAddress(sym, O->addr) &&
         containsAddress(sym, O->addr + O->size - 1);
}

SymbolEntry *PropellerProfWriter::findSymbolAtAddress(uint64_t pid,
                                                      uint64_t originAddr) {
  uint64_t addr = adjustAddressForPIE(pid, originAddr);
  if (addr == INVALID_ADDRESS) return nullptr;
  auto u = addrMap.upper_bound(addr);
  if (u == addrMap.begin()) return nullptr;
  auto r = std::prev(u);

  // 99+% of the cases:
  if (r->second.size() == 1 && containsAddress(r->second.front(), addr))
    return *(r->second.begin());

  list<SymbolEntry *> candidates;
  for (auto &SymEnt : r->second)
    if (containsAddress(SymEnt, addr)) candidates.emplace_back(SymEnt);

  if (candidates.empty()) return nullptr;

  // Sort candidates by symbol size.
  candidates.sort([](const SymbolEntry *s1, const SymbolEntry *s2) {
    if (s1->size != s2->size) return s1->size < s2->size;
    return s1->name < s2->name;
  });

  // Return the smallest symbol that contains address.
  return *candidates.begin();
}

namespace {
struct DecOut {
} dec;

struct HexOut {
} hex;

struct Hex0xOut {
} hex0x;

struct SymBaseF {
  SymBaseF(const SymbolEntry &sym) : Symbol(sym) {}
  const SymbolEntry &Symbol;

  static SymbolEntry dummySymbolEntry;
};

SymbolEntry SymBaseF::dummySymbolEntry(0, "", SymbolEntry::AliasesTy(),
                                       0, 0,  0);

struct SymNameF : public SymBaseF {
  SymNameF(const SymbolEntry &sym) : SymBaseF(sym), name("") {}
  SymNameF(const SymbolEntry *sym) : SymBaseF(*sym), name("") {}
  // Use explicit to prevent StringRef being casted to SymNameF.
  explicit SymNameF(const StringRef N)
      : SymBaseF(SymBaseF::dummySymbolEntry), name(N) {}

  StringRef name;
};

struct SymOrdinalF : public SymBaseF {
  SymOrdinalF(const SymbolEntry &sym) : SymBaseF(sym) {}
  SymOrdinalF(const SymbolEntry *sym) : SymBaseF(*sym) {}
};

struct SymSizeF : public SymBaseF {
  SymSizeF(const SymbolEntry &sym) : SymBaseF(sym) {}
};

struct SymShortF : public SymBaseF {
  SymShortF(const SymbolEntry &sym) : SymBaseF(sym) {}
  SymShortF(const SymbolEntry *sym) : SymBaseF(*sym) {}
};

struct countF {
  countF(uint64_t c) : cnt(c){};
  uint64_t cnt;
};

struct CommaF {
  CommaF(uint64_t v) : value(v) {}
  uint64_t value;
};

struct PercentageF {
  PercentageF(double d) : value(d) {}
  PercentageF(uint64_t a, uint64_t b) : value((double)a / (double)b) {}
  template<class Clazz>
  PercentageF(Clazz &c, uint64_t b) : value((double)c.size() / (double)b) {}
  template<class Clazz, class D>
  PercentageF(Clazz &c, D &d) : value((double)c.size() / (double)d.size()) {}
  double value;
};

struct BuildIdWrapper {
  BuildIdWrapper(const quipper::PerfDataProto_PerfBuildID &buildId)
      : data(buildId.build_id_hash().c_str()) {}

  BuildIdWrapper(const char *p) : data(p) {}

  const char *data;
};

static std::ostream &operator<<(std::ostream &out, const struct DecOut &) {
  return out << std::dec << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const struct HexOut &) {
  return out << std::hex << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const struct Hex0xOut &) {
  return out << std::hex << std::showbase;
}

static std::ostream &operator<<(std::ostream &out, const SymNameF &nameF) {
  if (!nameF.name.empty()) {
    // In this case, we only care about name field.
    // name is the form of "aaaa.BB.funcname" or "funcname".
    auto r = nameF.name.split(lld::propeller::BASIC_BLOCK_SEPARATOR);
    if (r.second.empty())
      out << r.first.str();
    else
      out << dec << r.first.size() << lld::propeller::BASIC_BLOCK_SEPARATOR
          << r.second.str();
    return out;
  }
  auto &sym = nameF.Symbol;
  if (sym.bbTag) {
    out << dec << sym.name.size() << lld::propeller::BASIC_BLOCK_SEPARATOR
        << (sym.containingFunc ? sym.containingFunc->name.str() : "null_func");
  } else {
    out << sym.name.str().c_str();
    for (auto a : sym.aliases) out << "/" << a.str();
  }
  return out;
}

static std::ostream &operator<<(std::ostream &out,
                                const SymOrdinalF &ordinalF) {
  return out << dec << ordinalF.Symbol.ordinal;
}

static std::ostream &operator<<(std::ostream &out, const SymSizeF &sizeF) {
  return out << hex << sizeF.Symbol.size;
}

static std::ostream &operator<<(std::ostream &out, const SymShortF &symSF) {
  return out << "symbol '" << SymNameF(symSF.Symbol) << "@" << hex0x
             << symSF.Symbol.addr << "'";
}

static std::ostream &operator<<(std::ostream &out, const countF &countF) {
  return out << dec << countF.cnt;
}

static std::ostream &operator<<(std::ostream &os, const MMapEntry &me) {
  return os << "[" << hex0x << me.loadAddr << ", " << hex0x << me.getEndAddr()
            << "] (PgOff=" << hex0x << me.pageOffset << ", size=" << hex0x
            << me.loadSize << ")";
};

static std::ostream &operator<<(std::ostream &out, const BuildIdWrapper &bw) {
  for (int i = 0; i < quipper::kBuildIDArraySize; ++i) {
    out << std::setw(2) << std::setfill('0') << std::hex
        << ((int)(bw.data[i]) & 0xFF);
  }
  return out;
}

// Output integer numbers in "," separated format.
static std::ostream &operator<<(std::ostream &out, const CommaF &cf) {
  std::list<int> seg;
  uint64_t value = cf.value;
  while (value) {
    seg.insert(seg.begin(), value % 1000);
    value /= 1000;
  }
  if (seg.empty()) seg.insert(seg.begin(), 0);
  auto of = out.fill();
  auto ow = out.width();
  auto i = seg.begin();
  out << std::setfill('\0') << *i;
  for (++i; i != seg.end(); ++i)
    out << "," << std::setw(3) << std::setfill('0') << dec << *i;
  out.fill(of);
  out.width(ow);
  return out;
}

static std::ostream &operator<<(std::ostream &out, const PercentageF &pf) {
  out << std::setprecision(3);
  out << (pf.value * 100) << '%';
  return out;
}

}  // namespace

bool PropellerProfWriter::write() {
  if (!initBinaryFile() || !findBinaryBuildId() || !populateSymbolMap())
    return false;

  std::fstream::pos_type partBegin, partEnd, partNew;
  {
    ofstream fout(propOutFileName);
    if (fout.bad()) {
      LOG(ERROR) << "Failed to open '" << propOutFileName << "' for writing.";
      return false;
    }
    writeOuts(fout);
    partNew = fout.tellp();
    writeSymbols(fout);
    if (!parsePerfData()) return false;
    writeBranches(fout);
    writeFallthroughs(fout);
    partBegin = fout.tellp();
    writeHotFuncAndBBList(fout);
    partEnd = fout.tellp();
  }
  // This must be done after "fout" is closed.
  if (!reorderSections(partBegin, partEnd, partNew)) {
    LOG(ERROR)
        << "Warn: failed to reorder propeller section, performance may suffer.";
  }
  summarize();
  return true;
}

// Move everything [partBegin, partEnd) -> partBegin.
bool PropellerProfWriter::reorderSections(int64_t partBegin, int64_t partEnd,
                                          int64_t partNew) {
  struct FdWrapper {
    FdWrapper(int fd) : v(fd) {}
    ~FdWrapper() { if (v != -1) close(v); }
    int v;
  };
  auto partLength = partEnd - partBegin;
  FdWrapper fd(open(propOutFileName.c_str(), O_RDWR));
  if (fd.v == -1) return false;
  unique_ptr<char> tmp(new char[partLength]);
  if (!tmp) return false;
  char *fileMem =
      static_cast<char *>(mmap(NULL, partEnd, PROT_WRITE, MAP_SHARED, fd.v, 0));
  if (MAP_FAILED == fileMem) return false;
  memcpy(tmp.get(), fileMem + partBegin, partLength);
  memmove(fileMem + partLength + partNew, fileMem + partNew,
          partBegin - partNew);
  memcpy(fileMem + partNew, tmp.get(), partLength);
  munmap(fileMem, partEnd);
  return true;
}

void PropellerProfWriter::summarize() {
  LOG(INFO) << "Wrote propeller profile (" << perfDataFileParsed << " file(s), "
            << CommaF(symbolsWritten) << " syms, " << CommaF(branchesWritten)
            << " branches, " << CommaF(fallthroughsWritten)
            << " fallthroughs) to " << propOutFileName;

  LOG(INFO) << CommaF(countersNotAddressed) << " of " << CommaF(totalCounters)
            << " branch entries are not mapped ("
            << PercentageF(countersNotAddressed, totalCounters) << ").";

  LOG(INFO) << CommaF(crossFunctionCounters) << " of " << CommaF(totalCounters)
            << " branch entries are cross function (" << std::setprecision(3)
            << PercentageF(crossFunctionCounters, totalCounters) << ").";

  LOG(INFO) << CommaF(fallthroughStartEndInDifferentFuncs) << " of "
            << CommaF(fallthroughCalculationNumber)
            << " fallthroughs are discareded because from and to are from "
               "different functions ("
            << PercentageF(fallthroughStartEndInDifferentFuncs,
                           fallthroughCalculationNumber)
            << ").";

  uint64_t totalBBsWithinFuncsWithProf = 0;
  uint64_t numBBsWithProf = 0;
  set<uint64_t> funcsWithProf;
  for (auto &se : hotSymbols) {
    if (funcsWithProf.insert(se->containingFunc->ordinal).second)
      totalBBsWithinFuncsWithProf += funcBBCounter[se->containingFunc->ordinal];
    if (se->bbTag)
      ++numBBsWithProf;
  }
  uint64_t totalFuncs = 0;
  uint64_t totalBBsAll = 0;
  for (auto &p : this->symbolNameMap) {
    SymbolEntry *sym = p.second.get();
    if (sym->bbTag)
      ++totalBBsAll;
    else
      ++totalFuncs;
  }
  LOG(INFO) << CommaF(totalFuncs) << " functions, "
            << CommaF(funcsWithProf.size()) << " functions with prof ("
            << PercentageF(funcsWithProf, totalFuncs) << ")"
            << ", " << CommaF(totalBBsAll) << " BBs (average "
            << totalBBsAll / totalFuncs << " BBs per func), "
            << CommaF(totalBBsWithinFuncsWithProf) << " BBs within hot funcs ("
            << PercentageF(totalBBsWithinFuncsWithProf, totalBBsAll) << "), "
            << CommaF(numBBsWithProf) << " BBs with prof (include "
            << CommaF(extraBBsIncludedInFallthroughs)
            << " BBs that are on the path of "
               "fallthroughs, total accounted for "
            << PercentageF(numBBsWithProf, totalBBsAll) << " of all BBs).";
}

void PropellerProfWriter::writeOuts(ofstream &fout) {
  set<string> paths{FLAGS_match_mmap_file, binaryMMapName, binaryFileName};
  set<string> nameMatches;
  for (const auto &v : paths)
    if (!v.empty()) nameMatches.insert(llvm::sys::path::filename(v).str());

  for (const auto &v : nameMatches)
    if (!v.empty()) fout << "@" << v << std::endl;
}

void PropellerProfWriter::writeHotFuncAndBBList(ofstream &fout) {
  SymbolEntry *LastFuncSymbol = nullptr;
  uint32_t bbCount = 0;
  for (auto *se : hotSymbols)
    if (se->bbTag) {
      if (LastFuncSymbol != se->containingFunc) {
        fout << "!" << SymNameF(se->containingFunc) << std::endl;
        // If we haven't output any BB symbols for LastFuncSymbol, we then
        // output "!!0" which means the entry block is hot, before we switch
        // to processing another hot function.
        if (!bbCount && LastFuncSymbol)
          fout << "!!0" << std::endl;
        LastFuncSymbol = se->containingFunc;
      }
      fout << "!!" << se->name.size() << std::endl;
      ++bbCount;
    } else {
      fout << "!" << SymNameF(se) << std::endl;
      LastFuncSymbol = se;
    }
}

void PropellerProfWriter::writeSymbols(ofstream &fout) {
  this->symbolsWritten = 0;
  uint64_t symbolOrdinal = 0;
  fout << "Symbols" << std::endl;
  for (auto &le : addrMap) {
    // Tricky case here:
    // In the same address we have:
    //    foo.bb.1
    //    foo
    // So we first output foo.bb.1, and at this time
    //   foo.bb.1->containingFunc->Index == 0.
    // We output 0.1, wrong!.
    // to handle this, we sort le by bbTag:
    if (le.second.size() > 1) {
      le.second.sort([](SymbolEntry *s1, SymbolEntry *s2) {
        if (s1->bbTag != s2->bbTag) {
          return !s1->bbTag;
        }
        // order irrelevant, but choose a stable relation.
        if (s1->size != s2->size) return s1->size < s2->size;
        return s1->name < s2->name;
      });
    }
    // Then apply ordial to all before accessing.
    for (auto *sePtr : le.second) {
      sePtr->ordinal = ++symbolOrdinal;
    }
    for (auto *sePtr : le.second) {
      SymbolEntry &se = *sePtr;
      fout << SymOrdinalF(se) << " " << SymSizeF(se) << " ";
      ++this->symbolsWritten;
      if (se.bbTag) {
        fout << SymOrdinalF(se.containingFunc) << ".";
        StringRef bbIndex = se.name;
        fout << dec << (uint64_t)(bbIndex.size());
        if (bbIndex.front() != 'a')
          fout << bbIndex.front();
        fout << std::endl;
        ++funcBBCounter[se.containingFunc->ordinal];
      } else {
        fout << "N" << SymNameF(se) << std::endl;
      }
    }
  }
}

void PropellerProfWriter::writeBranches(std::ofstream &fout) {
  this->branchesWritten = 0;
  fout << "Branches" << std::endl;
  auto recordHotSymbol = [this](SymbolEntry *sym) {
    if (!sym || !(sym->containingFunc) || sym->containingFunc->name.empty())
      return;
    if (sym->isLandingPadBlock())
      // Landing pads are always treated as not hot.
      LOG(WARNING) << "*** HOT LANDING PAD: " << sym->name.str() << "\t"
                   << sym->containingFunc->name.str() << "\n";
    else
      // Dups are properly handled by set.
      hotSymbols.insert(sym);
  };

  using BrCntSummationKey = tuple<SymbolEntry *, SymbolEntry *, char>;
  struct BrCntSummationKeyComp {
    bool operator()(const BrCntSummationKey &k1,
                    const BrCntSummationKey &k2) const {
      SymbolEntry *From1, *From2, *To1, *To2;
      char T1, T2;
      std::tie(From1, To1, T1) = k1;
      std::tie(From2, To2, T2) = k2;
      if (From1->ordinal != From2->ordinal)
        return From1->ordinal < From2->ordinal;
      if (To1->ordinal != To2->ordinal) return To1->ordinal < To2->ordinal;
      return T1 < T2;
    }
  };
  using BrCntSummationTy =
      map<BrCntSummationKey, uint64_t, BrCntSummationKeyComp>;
  BrCntSummationTy brCntSummation;

  totalCounters = 0;
  countersNotAddressed = 0;
  crossFunctionCounters = 0;
  // {pid: {<from, to>: count}}
  for (auto &bcPid : branchCountersByPid) {
    const uint64_t pid = bcPid.first;
    auto &BC = bcPid.second;
    for (auto &EC : BC) {
      const uint64_t from = EC.first.first;
      const uint64_t to = EC.first.second;
      const uint64_t cnt = EC.second;
      auto *fromSym = findSymbolAtAddress(pid, from);
      auto *toSym = findSymbolAtAddress(pid, to);
      const uint64_t adjustedFrom = adjustAddressForPIE(pid, from);
      const uint64_t adjustedTo = adjustAddressForPIE(pid, to);

      recordHotSymbol(fromSym);
      recordHotSymbol(toSym);

      totalCounters += cnt;

      if (!toSym || !fromSym) {
        countersNotAddressed += cnt;
        continue;
      }

      // If this is a return to the beginning of a basic block, change the toSym
      // to the basic block just before and add fallthrough between the two
      // symbols.
      // After this code executes, adjustedTo can never be the beginning of a
      // basic block for returns.
      if ((fromSym->isReturnBlock() ||
           toSym->containingFunc->addr != fromSym->containingFunc->addr) &&
          toSym->containingFunc->addr != adjustedTo &&  // Not a call
          // Jump to the beginning of the basicblock
          adjustedTo == toSym->addr) {
        auto prevAddr = std::prev(addrMap.find(adjustedTo))->first;
        auto *callSiteSym =
            findSymbolAtAddress(pid, to - (adjustedTo - prevAddr));
        if (callSiteSym) {
          recordHotSymbol(callSiteSym);
          // Account for the fall-through between callSiteSym and toSym.
          fallthroughCountersBySymbol[make_pair(callSiteSym, toSym)] += cnt;
          // Reassign toSym to be the actuall callsite symbol entry.
          toSym = callSiteSym;
        } else
          LOG(WARNING) << "*** Internal error: Could not find the right "
                          "callSiteSym for : "
                       << adjustedTo;
      }

      if (fromSym->containingFunc != toSym->containingFunc)
        crossFunctionCounters += cnt;

      char type = ' ';
      if (toSym->containingFunc->addr == adjustedTo) {
        type = 'C';
      } else if (adjustedTo != toSym->addr) {
        type = 'r';
      }
      brCntSummation[std::make_tuple(fromSym, toSym, type)] += cnt;
    }
  }

  for (auto &brEnt : brCntSummation) {
    SymbolEntry *fromSym, *toSym;
    char type;
    std::tie(fromSym, toSym, type) = brEnt.first;
    uint64_t cnt = brEnt.second;
    fout << SymOrdinalF(fromSym) << " " << SymOrdinalF(toSym) << " "
         << countF(cnt);
    if (type != ' ') {
      fout << ' ' << type;
    }
    fout << std::endl;
    ++this->branchesWritten;
  }
}

// Compute fallthrough BBs from "from" -> "to", and place them in "path".
// ("from" and "to" are excluded)
bool PropellerProfWriter::calculateFallthroughBBs(
    SymbolEntry *from, SymbolEntry *to, std::vector<SymbolEntry *> &path) {
  path.clear();
  ++fallthroughCalculationNumber;
  if (from == to) return true;
  if (from->addr > to->addr) {
    LOG(WARNING) << "*** Internal error: fallthrough path start address is "
                  "larger than end address. ***";
    return false;
  }

  auto p = addrMap.find(from->addr), q = addrMap.find(to->addr),
       e = addrMap.end();
  if (p == e || q == e) {
    LOG(FATAL) << "*** Internal error: invalid symbol in fallthrough pair. ***";
    return false;
  }
  q = std::next(q);
  if (from->containingFunc != to->containingFunc) {
    LOG(ERROR) << "fallthrough (" << SymShortF(from) << " -> "
               << SymShortF(to)
               << ") does not start and end within the same faunction.";
    ++fallthroughStartEndInDifferentFuncs;
    return false;
  }
  auto func = from->containingFunc;
  auto i = p;
  for (++i; i != q && i != e; ++i) {
    SymbolEntry *lastFoundSymbol = nullptr;
    for (auto *se : i->second) {
      if (se->bbTag && se->containingFunc == func) {
        if (lastFoundSymbol) {
          LOG(ERROR) << "fallthrough (" << SymShortF(from) << " -> "
                     << SymShortF(to) << ") contains ambiguous "
                     << SymShortF(se) << " and " << SymShortF(lastFoundSymbol)
                     << ".";
        }
        // Add the symbol entry to path, unless it is the the "to" symbol.
        if (se != to)
          path.emplace_back(se);
        lastFoundSymbol = se;
      }
    }
    if (!lastFoundSymbol) {
      LOG(ERROR) << "failed to find a BB for "
                 << "fallthrough (" << SymShortF(*from) << " -> "
                 << SymShortF(to) << ").";
      return false;
    }
  }
  if (path.size() >= 200)
    LOG(ERROR) << "too many BBs along fallthrough (" << SymShortF(from)
               << " -> " << SymShortF(to) << "): " << dec << path.size()
               << " BBs.";
  return true;
}

void PropellerProfWriter::writeFallthroughs(std::ofstream &fout) {
  // caPid: {pid, <<from_addr, to_addr>, counter>}
  for (auto &caPid : fallthroughCountersByPid) {
    uint64_t pid = caPid.first;
    for (auto &ca : caPid.second) {
      const uint64_t cnt = ca.second;
      auto *fromSym = findSymbolAtAddress(pid, ca.first.first);
      auto *toSym = findSymbolAtAddress(pid, ca.first.second);
      if (fromSym && toSym)
        fallthroughCountersBySymbol[std::make_pair(fromSym, toSym)] += cnt;
    }
  }

  fout << "Fallthroughs" << std::endl;
  extraBBsIncludedInFallthroughs = 0;
  fallthroughStartEndInDifferentFuncs = 0;
  fallthroughCalculationNumber = 0;
  for (auto &fc : fallthroughCountersBySymbol) {
    std::vector<SymbolEntry *> path;
    SymbolEntry *fallthroughFrom = fc.first.first,
                *fallthroughTo = fc.first.second;
    if (fallthroughFrom != fallthroughTo &&
        calculateFallthroughBBs(fallthroughFrom, fallthroughTo, path)) {
      totalCounters += (path.size() + 1) * fc.second;
      // Note, fallthroughFrom/to are not included in "path".
      hotSymbols.insert(fallthroughFrom);
      hotSymbols.insert(fallthroughTo);
      for (auto *sym : path) {
        if (sym->isLandingPadBlock()) {
          LOG(WARNING) << "*** HOT LANDING PAD: " << sym->name.str() << "\t"
                       << sym->containingFunc->name.str() << "\n";
        }
        extraBBsIncludedInFallthroughs += hotSymbols.insert(sym).second ? 1 : 0;
      }
    }

    fout << SymOrdinalF(*(fc.first.first)) << " "
         << SymOrdinalF(*(fc.first.second)) << " " << countF(fc.second)
         << std::endl;
  }
  this->fallthroughsWritten = fallthroughCountersBySymbol.size();
}

template <class ELFT>
bool fillELFPhdr(llvm::object::ELFObjectFileBase *ebFile,
                 map<uint64_t, uint64_t> &phdrLoadMap) {
  ELFObjectFile<ELFT> *eobj = dyn_cast<ELFObjectFile<ELFT>>(ebFile);
  if (!eobj) return false;
  const ELFFile<ELFT> *efile = eobj->getELFFile();
  if (!efile) return false;
  auto program_headers = efile->program_headers();
  if (!program_headers) return false;
  for (const typename ELFT::Phdr &phdr : *program_headers) {
    if (phdr.p_type == llvm::ELF::PT_LOAD &&
        (phdr.p_flags & llvm::ELF::PF_X) != 0) {
      auto e = phdrLoadMap.find(phdr.p_vaddr);
      if (e == phdrLoadMap.end()) {
        phdrLoadMap.emplace(phdr.p_vaddr, phdr.p_memsz);
      } else {
        if (e->second != phdr.p_memsz) {
          LOG(ERROR) << "Invalid phdr found in elf binary file.";
          return false;
        }
      }
    }
  }
  if (phdrLoadMap.empty()) {
    LOG(ERROR) << "No loadable and executable segments found in binary.";
    return false;
  }
  stringstream SS;
  SS << "Loadable and executable segments:\n";
  for (auto &Seg : phdrLoadMap) {
    SS << "\tvaddr=" << hex0x << Seg.first << ", memsz=" << hex0x << Seg.second
       << std::endl;
  }
  LOG(INFO) << SS.str();
  return true;
}

bool PropellerProfWriter::initBinaryFile() {
  auto FileOrError = llvm::MemoryBuffer::getFile(binaryFileName);
  if (!FileOrError) {
    LOG(ERROR) << "Failed to read file '" << binaryFileName << "'.";
    return false;
  }
  this->binaryFileContent = std::move(*FileOrError);

  auto ObjOrError = llvm::object::ObjectFile::createELFObjectFile(
      llvm::MemoryBufferRef(*(this->binaryFileContent)));
  if (!ObjOrError) {
    LOG(ERROR) << "Not a valid ELF file '" << binaryFileName << "'.";
    return false;
  }
  this->objFile = std::move(*ObjOrError);

  auto *ELFObjBase = dyn_cast<ELFObjectFileBase, ObjectFile>(objFile.get());
  binaryIsPIE = (ELFObjBase->getEType() == llvm::ELF::ET_DYN);
  if (binaryIsPIE) {
    const char *ELFIdent = binaryFileContent->getBufferStart();
    const char ELFClass = ELFIdent[4];
    const char ELFData = ELFIdent[5];
    if (ELFClass == 1 && ELFData == 1) {
      fillELFPhdr<llvm::object::ELF32LE>(ELFObjBase, phdrLoadMap);
    } else if (ELFClass == 1 && ELFData == 2) {
      fillELFPhdr<llvm::object::ELF32BE>(ELFObjBase, phdrLoadMap);
    } else if (ELFClass == 2 && ELFData == 1) {
      fillELFPhdr<llvm::object::ELF64LE>(ELFObjBase, phdrLoadMap);
    } else if (ELFClass == 2 && ELFData == 2) {
      fillELFPhdr<llvm::object::ELF64BE>(ELFObjBase, phdrLoadMap);
    } else {
      assert(false);
    }
  }
  LOG(INFO) << "'" << this->binaryFileName
            << "' is PIE binary: " << binaryIsPIE;
  return true;
}

bool PropellerProfWriter::populateSymbolMap() {
  auto Symbols = objFile->symbols();
  const set<StringRef> ExcludedSymbols{"__cxx_global_array_dtor"};
  for (const auto &sym : Symbols) {
    auto AddrR = sym.getAddress();
    auto SecR = sym.getSection();
    auto NameR = sym.getName();
    auto TypeR = sym.getType();

    if (!(AddrR && *AddrR && SecR && (*SecR)->isText() && NameR && TypeR))
      continue;

    StringRef name = *NameR;
    if (name.empty()) continue;
    uint64_t addr = *AddrR;
    uint8_t type(*TypeR);
    llvm::object::ELFSymbolRef ELFSym(sym);
    uint64_t size = ELFSym.getSize();

    StringRef BBFunctionName;
    bool isFunction = (type == llvm::object::SymbolRef::ST_Function);
    bool isBB = SymbolEntry::isBBSymbol(name, &BBFunctionName);

    if (!isFunction && !isBB) continue;
    if (ExcludedSymbols.find(isBB ? BBFunctionName : name) !=
        ExcludedSymbols.end()) {
      continue;
    }

    auto &L = addrMap[addr];
    if (!L.empty()) {
      // If we already have a symbol at the same address, merge
      // them together.
      SymbolEntry *SymbolIsAliasedWith = nullptr;
      for (auto *sym : L) {
        if (sym->size == size || (sym->isFunction() && isFunction)) {
          // Make sure name and Aliased name are both BB or both NON-BB.
          if (SymbolEntry::isBBSymbol(sym->name) !=
              SymbolEntry::isBBSymbol(name)) {
            LOG(INFO) << "Dropped symbol: '" << SymNameF(name)
                      << "'. The symbol conflicts with another symbol on the "
                         "same address: "
                      << hex0x << addr;
            continue;
          }
          sym->aliases.push_back(name);
          if (sym->size < size) sym->size = size;
          if (!sym->isFunction() &&
              type == llvm::object::SymbolRef::ST_Function) {
            // If any of the aliased symbols is a function, promote the whole
            // group to function.
            sym->type = llvm::object::SymbolRef::ST_Function;
          }
          SymbolIsAliasedWith = sym;
          break;
        }
      }
      if (SymbolIsAliasedWith) continue;
    }

    // Delete symbol with same name from symbolNameMap and addrMap.
    map<StringRef, unique_ptr<SymbolEntry>>::iterator ExistingNameR =
        symbolNameMap.find(name);
    if (ExistingNameR != symbolNameMap.end()) {
      LOG(INFO) << "Dropped duplicate symbol \"" << SymNameF(name) << "\". "
                << "Consider using \"-funique-internal-funcnames\" to "
                   "dedupe internal function names.";
      map<uint64_t, list<SymbolEntry *>>::iterator ExistingLI =
          addrMap.find(ExistingNameR->second->addr);
      if (ExistingLI != addrMap.end()) {
        ExistingLI->second.remove_if(
            [&name](SymbolEntry *sym) { return sym->name == name; });
      }
      symbolNameMap.erase(ExistingNameR);
      continue;
    }

    SymbolEntry *NewSymbolEntry =
        new SymbolEntry(0, name, SymbolEntry::AliasesTy(), addr, size, type);
    L.push_back(NewSymbolEntry);
    NewSymbolEntry->bbTag = SymbolEntry::isBBSymbol(name);
    // Set the BB Tag type according to the first character of the symbol name.
    if (NewSymbolEntry->bbTag)
      NewSymbolEntry->bbTagType = SymbolEntry::toBBTagType(name.front());
    else
      NewSymbolEntry->bbTagType = SymbolEntry::BB_NONE;
    symbolNameMap.emplace(std::piecewise_construct, std::forward_as_tuple(name),
                          std::forward_as_tuple(NewSymbolEntry));
  }  // End of iterating all symbols.

  // Now scan all the symbols in address order to create function <-> bb
  // relationship.
  uint64_t BBSymbolDropped = 0;
  decltype(addrMap)::iterator LastFuncPos = addrMap.end();
  for (auto p = addrMap.begin(), q = addrMap.end(); p != q; ++p) {
    int FuncCount = 0;
    for (auto *sym : p->second) {
      if (sym->isFunction() && !sym->bbTag) {
        if (++FuncCount > 1) {
          // 2 different functions start at the same address, but with different
          // sizes, this is not supported.
          LOG(ERROR)
              << "Analyzing failure: at address 0x" << hex << p->first
              << ", there are more than 1 functions that have different sizes.";
          return false;
        }
        LastFuncPos = p;
      }
    }

    if (LastFuncPos == addrMap.end()) continue;
    for (auto *sym : p->second) {
      if (!sym->bbTag) {
        // Set a function's wrapping function to itself.
        sym->containingFunc = sym;
        continue;
      }
      // This is a bb symbol, find a wrapping func for it.
      SymbolEntry *containingFunc = nullptr;
      for (SymbolEntry *FP : LastFuncPos->second) {
        if (FP->isFunction() && !FP->bbTag && containsAnotherSymbol(FP,sym) &&
            isFunctionForBBName(FP, sym->name)) {
          if (containingFunc == nullptr) {
            containingFunc = FP;
          } else {
            // Already has a containing function, so we have at least 2
            // different functions with different sizes but start at the same
            // address, impossible?
            LOG(ERROR) << "Analyzing failure: at address 0x" << hex
                       << LastFuncPos->first
                       << ", there are 2 different functions: "
                       << SymNameF(containingFunc) << " and "
                       << SymNameF(FP);
            return false;
          }
        }
      }
      if (!containingFunc) {
        // Disambiguate the following case:
        // 0x10 foo       size = 2
        // 0x12 foo.bb.1  size = 2
        // 0x14 foo.bb.2  size = 0
        // 0x14 bar  <- LastFuncPos set is to bar.
        // 0x14 bar.bb.1
        // In this scenario, we seek lower address.
        auto T = LastFuncPos;
        int FunctionSymbolSeen = 0;
        while (T != addrMap.begin()) {
          T = std::prev(T);
          bool isFunction = false;
          for (auto *KS : T->second) {
            isFunction |= KS->isFunction();
            if (KS->isFunction() && !KS->bbTag &&
                containsAnotherSymbol(KS, sym) &&
                isFunctionForBBName(KS, sym->name)) {
              containingFunc = KS;
              break;
            }
          }
          FunctionSymbolSeen += isFunction ? 1 : 0;
          // Only go back for at most 2 function symbols.
          if (FunctionSymbolSeen > 2) break;
        }
      }
      sym->containingFunc = containingFunc;
      if (sym->containingFunc == nullptr) {
        LOG(ERROR) << "Dropped bb symbol without any wrapping function: \""
                   << SymShortF(sym) << "\"";
        ++BBSymbolDropped;
        addrMap.erase(p--);
        break;
      } else {
        if (!isFunctionForBBName(containingFunc, sym->name)) {
          LOG(ERROR) << "Internal check warning: \n"
                     << "sym: " << SymShortF(sym) << "\n"
                     << "func: " << SymShortF(sym->containingFunc);
          return false;
        }
      }
      
      // Now here is the tricky thing to fix:
      //    Wrapping func _zfooc2/_zfooc1/_zfooc3
      //    bbname: a.BB._zfooc1
      //
      // We want to make sure the primary name (the name first appears in the
      // alias) matches the bb name, so we change the wrapping func aliases to:
      //    _zfooc1/_zfooc2/_zfooc3
      // By doing this, the wrapping func matches "a.BB._zfooc1" correctly.
      //
      if (!containingFunc->aliases.empty()) {
        auto a = sym->name.split(lld::propeller::BASIC_BLOCK_SEPARATOR);
        auto ExpectFuncName = a.second;
        auto &aliases = containingFunc->aliases;
        if (ExpectFuncName != containingFunc->name) {
          SymbolEntry::AliasesTy::iterator p, q;
          for (p = aliases.begin(), q = aliases.end(); p != q; ++p)
            if (*p == ExpectFuncName) break;

          if (p == q) {
            LOG(ERROR) << "Internal check error: bb symbol '" << sym->name.str()
                       << "' does not have a valid wrapping function.";
            return false;
          }
          StringRef OldName = containingFunc->name;
          containingFunc->name = *p;
          aliases.erase(p);
          aliases.push_back(OldName);
        }
      }

      // Replace the whole name (e.g. "aaaa.BB.foo" with "aaaa" only);
      StringRef FName, BName;
      bool r = SymbolEntry::isBBSymbol(sym->name, &FName, &BName);
      (void)(r);
      assert(r);
      if (FName != sym->containingFunc->name) {
        LOG(ERROR) << "Internal check error: bb symbol '" << sym->name.str()
                       << "' does not have a valid wrapping function.";
      }
      sym->name = BName;
    }  // End of iterating p->second
  }    // End of iterating addrMap.
  if (BBSymbolDropped)
    LOG(INFO) << "Dropped " << dec << CommaF(BBSymbolDropped)
              << " bb symbol(s).";
  return true;
}

bool PropellerProfWriter::parsePerfData() {
  this->perfDataFileParsed = 0;
  StringRef FN(perfFileName);
  while (!FN.empty()) {
    StringRef PerfName;
    std::tie(PerfName, FN) = FN.split(',');
    if (!parsePerfData(PerfName.str())) {
      return false;
    }
    ++this->perfDataFileParsed;
  }
  LOG(INFO) << "Processed " << perfDataFileParsed << " perf file(s).";
  return true;
}

bool PropellerProfWriter::parsePerfData(const string &PName) {
  quipper::PerfReader PR;
  if (!PR.ReadFile(PName)) {
    LOG(ERROR) << "Failed to read perf data file: " << PName;
    return false;
  }

  quipper::PerfParser Parser(&PR);
  if (!Parser.ParseRawEvents()) {
    LOG(ERROR) << "Failed to parse perf raw events for perf file: '" << PName
               << "'.";
    return false;
  }

  if (!FLAGS_ignore_build_id) {
    if (!setupBinaryMMapName(PR, PName)) {
      return false;
    }
  }

  if (!setupMMaps(Parser, PName)) {
    LOG(ERROR) << "Failed to find perf mmaps for binary '" << binaryFileName
               << "'.";
    return false;
  }

  return aggregateLBR(Parser);
}

bool PropellerProfWriter::setupMMaps(quipper::PerfParser &Parser,
                                     const string &PName) {
  // Depends on the binary file name, if
  //   - it is absolute, compares it agains the full path
  //   - it is relative, only compares the file name part
  // Note: CompFunc is constructed in a way so that there is no branch /
  // conditional test inside the function.
  struct BinaryNameComparator {
    BinaryNameComparator(const string &binaryFileName) {
      if (llvm::sys::path::is_absolute(binaryFileName)) {
        ComparePart = StringRef(binaryFileName);
        PathChanger = NullPathChanger;
      } else {
        ComparePart = llvm::sys::path::filename(binaryFileName);
        PathChanger = NameOnlyPathChanger;
      }
    }

    bool operator()(const string &path) {
      return ComparePart == PathChanger(path);
    }

    StringRef ComparePart;
    std::function<StringRef(const string &)> PathChanger;

    static StringRef NullPathChanger(const string &sym) { return StringRef(sym); }
    static StringRef NameOnlyPathChanger(const string &sym) {
      return llvm::sys::path::filename(StringRef(sym));
    }
  } CompFunc(FLAGS_match_mmap_file.empty()
                 ? (this->binaryMMapName.empty() ? binaryFileName
                                                 : this->binaryMMapName)
                 : FLAGS_match_mmap_file);

  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() != quipper::PerfDataProto_PerfEvent::kMmapEvent)
      continue;

    const quipper::PerfDataProto_MMapEvent &mmap = EPtr->mmap_event();
    if (!mmap.has_filename()) continue;

    const string &MMapFileName = mmap.filename();
    if (!CompFunc(MMapFileName) || !mmap.has_start() || !mmap.has_len() ||
        !mmap.has_pid())
      continue;

    if (this->binaryMMapName.empty()) {
      this->binaryMMapName = MMapFileName;
    } else if (binaryMMapName != MMapFileName) {
      LOG(ERROR) << "'" << binaryFileName
                 << "' is not specific enough. It matches both '"
                 << binaryMMapName << "' and '" << MMapFileName
                 << "' in the perf data file '" << PName
                 << "'. Consider using absolute file name.";
      return false;
    }
    uint64_t loadAddr = mmap.start();
    uint64_t loadSize = mmap.len();
    uint64_t pageOffset = mmap.has_pgoff() ? mmap.pgoff() : 0;

    // For the same binary, mmap can only be different if it is a PIE binary. So
    // for non-PIE binaries, we check all MMaps are equal and merge them into
    // binaryMMapByPid[0].
    uint64_t MPid = binaryIsPIE ? mmap.pid() : 0;
    set<MMapEntry> &LoadMap = binaryMMapByPid[MPid];
    // Check for mmap conflicts.
    if (!checkBinaryMMapConflictionAndEmplace(loadAddr, loadSize, pageOffset,
                                              LoadMap)) {
      stringstream SS;
      SS << "Found conflict mmap event: "
         << MMapEntry{loadAddr, loadSize, pageOffset}
         << ". Existing mmap entries: " << std::endl;
      for (auto &EM : LoadMap) {
        SS << "\t" << EM << std::endl;
      }
      LOG(ERROR) << SS.str();
      return false;
    }
  }  // End of iterating mmap events.

  if (!std::accumulate(
          binaryMMapByPid.begin(), binaryMMapByPid.end(), 0,
          [](uint64_t V, const decltype(binaryMMapByPid)::value_type &sym)
              -> uint64_t { return V + sym.second.size(); })) {
    LOG(ERROR) << "Failed to find mmap entries in '" << PName << "' for '"
               << binaryFileName << "'.";
    return false;
  }
  for (auto &M : binaryMMapByPid) {
    stringstream SS;
    SS << "Found mmap in '" << PName << "' for binary: '" << binaryFileName
       << "', pid=" << dec << M.first << " (0 for non-pie executables)"
       << std::endl;
    for (auto &N : M.second) {
      SS << "\t" << N << std::endl;
    }
    LOG(INFO) << SS.str();
  }
  return true;
}

bool PropellerProfWriter::checkBinaryMMapConflictionAndEmplace(
    uint64_t loadAddr, uint64_t loadSize, uint64_t pageOffset,
    set<MMapEntry> &M) {
  for (const MMapEntry &e : M) {
    if (e.loadAddr == loadAddr && e.loadSize == loadSize &&
        e.pageOffset == pageOffset)
      return true;
    if (!((loadAddr + loadSize <= e.loadAddr) ||
          (e.loadAddr + e.loadSize <= loadAddr)))
      return false;
  }
  auto r = M.emplace(loadAddr, loadSize, pageOffset);
  assert(r.second);
  return true;
}

bool PropellerProfWriter::setupBinaryMMapName(quipper::PerfReader &PR,
                                              const string &PName) {
  this->binaryMMapName = "";
  if (FLAGS_ignore_build_id || this->binaryBuildId.empty()) {
    return true;
  }
  list<pair<string, string>> ExistingBuildIds;
  for (const auto &buildId : PR.build_ids()) {
    if (buildId.has_filename() && buildId.has_build_id_hash()) {
      string PerfBuildId = buildId.build_id_hash();
      quipper::PerfizeBuildIDString(&PerfBuildId);
      ExistingBuildIds.emplace_back(buildId.filename(), PerfBuildId);
      if (PerfBuildId == this->binaryBuildId) {
        this->binaryMMapName = buildId.filename();
        LOG(INFO) << "Found file with matching buildId in perf file '" << PName
                  << "': " << this->binaryMMapName;
        return true;
      }
    }
  }
  stringstream SS;
  SS << "No file with matching buildId in perf data '" << PName
     << "', which contains the following <file, buildid>:" << std::endl;
  for (auto &p : ExistingBuildIds) {
    SS << "\t" << p.first << ": " << BuildIdWrapper(p.second.c_str())
       << std::endl;
  }
  LOG(INFO) << SS.str();
  return false;
}

bool PropellerProfWriter::aggregateLBR(quipper::PerfParser &Parser) {
  uint64_t brstackCount = 0;
  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() !=
        quipper::PerfDataProto_PerfEvent::kSampleEvent)
      continue;

    auto &SEvent = EPtr->sample_event();
    if (!SEvent.has_pid()) continue;
    auto BRStack = SEvent.branch_stack();
    if (BRStack.empty()) continue;
    uint64_t pid = binaryIsPIE ? SEvent.pid() : 0;
    if (binaryMMapByPid.find(pid) == binaryMMapByPid.end()) continue;
    auto &BranchCounters = branchCountersByPid[pid];
    auto &FallthroughCounters = fallthroughCountersByPid[pid];
    uint64_t LastFrom = INVALID_ADDRESS;
    uint64_t LastTo = INVALID_ADDRESS;
    brstackCount += BRStack.size();
    std::vector<SymbolEntry *> SymSeq{};
    SymbolEntry *LastToSym = nullptr;
    for (int p = BRStack.size() - 1; p >= 0; --p) {
      const auto &BE = BRStack.Get(p);
      uint64_t from = BE.from_ip();
      uint64_t to = BE.to_ip();
      // TODO: LBR sometimes duplicates the first entry by mistake. For now we
      // treat these to be true entries.

      // if (p == 0 && from == LastFrom && to == LastTo) {
      //  LOG(INFO) << "Ignoring duplicate LBR entry: 0x" << std::hex <<
      //             from
      //             << "-> 0x" << to << std::dec << "\n";
      //  continue;
      //}

      ++(BranchCounters[make_pair(from, to)]);
      if (LastTo != INVALID_ADDRESS && LastTo <= from)
        ++(FallthroughCounters[make_pair(LastTo, from)]);

      // Aggregate path profile information.
      if (FLAGS_gen_path_profile) {
        auto *fromSym = findSymbolAtAddress(pid, from);
        auto *toSym = findSymbolAtAddress(pid, to);
        if (LastTo != INVALID_ADDRESS && LastTo > from) {
          pathProfile.addSymSeq(SymSeq);
          SymSeq.clear();
          goto done_path;
        }

        if (p == 0 && from == LastFrom && to == LastTo) {
          // LOG(INFO) << "Ignoring duplicate LBR entry: 0x" << std::hex <<
          // from
          //           << "-> 0x" << to << std::dec << "\n";
          goto done_path;
        }
        if (fromSym && toSym) {
          SymSeq.push_back(fromSym);
          SymSeq.push_back(toSym);
        } else {
          pathProfile.addSymSeq(SymSeq);
          SymSeq.clear();
        }
      done_path:;
      }  // Done path profile

      LastTo = to;
      LastFrom = from;
    }  // end of iterating one br record
    if (FLAGS_gen_path_profile) {
      pathProfile.addSymSeq(SymSeq);
      SymSeq.clear();
    }
  }  // End of iterating all br records.
  if (brstackCount < 100) {
    LOG(ERROR) << "Too few brstack records (only " << brstackCount
               << " record(s) found), cannot continue.";
    return false;
  }
  LOG(INFO) << "Processed " << CommaF(brstackCount) << " lbr records.";
  if (FLAGS_gen_path_profile)
    for (auto i = pathProfile.maxPaths.begin(), J = pathProfile.maxPaths.end();
         i != J; ++i)
      if ((*i)->expandToIncludeFallthroughs(*this))
        std::cout << *(*i) << std::endl;

  return true;
}

bool PropellerProfWriter::findBinaryBuildId() {
  this->binaryBuildId = "";
  if (FLAGS_ignore_build_id) return true;
  bool BuildIdFound = false;
  for (auto &SR : objFile->sections()) {
    llvm::object::ELFSectionRef ESR(SR);
    StringRef SName;
    auto ExSecRefName = SR.getName();
    if (ExSecRefName) {
      SName = *ExSecRefName;
    } else {
      continue;
    }
    auto ExpectedSContents = SR.getContents();
    if (ESR.getType() == llvm::ELF::SHT_NOTE && SName == ".note.gnu.build-id" &&
        ExpectedSContents && !ExpectedSContents->empty()) {
      StringRef SContents = *ExpectedSContents;
      const unsigned char *p = SContents.bytes_begin() + 0x10;
      if (p >= SContents.bytes_end()) {
        LOG(INFO) << "Section '.note.gnu.build-id' does not contain valid "
                     "build id information.";
        return true;
      }
      string buildId((const char *)p, SContents.size() - 0x10);
      quipper::PerfizeBuildIDString(&buildId);
      this->binaryBuildId = buildId;
      LOG(INFO) << "Found Build Id in binary '" << binaryFileName
                << "': " << BuildIdWrapper(buildId.c_str());
      return true;
    }
  }
  LOG(INFO) << "No Build Id found in '" << binaryFileName << "'.";
  return true;  // always returns true
}

bool PathProfile::addSymSeq(vector<SymbolEntry *> &symSeq) {
  if (symSeq.size() < MIN_LENGTH) return false;
  auto Range = findPaths(symSeq);
  path P1(std::move(symSeq));
  for (auto i = Range.first; i != Range.second; ++i)
    if (i->second.mergeableWith(P1)) {
      path &mergeable = i->second;
      removeFromMaxPaths(mergeable);
      mergeable.merge(P1);
      addToMaxPaths(mergeable);
      return true;
    }

  // Insert "P1" into PathProfile.
  path::Key K = P1.pathKey();
  // std::cout << "Added path: " << P1 << std::endl;
  addToMaxPaths(Paths.emplace(K, std::move(P1))->second);
  return true;
}

bool path::expandToIncludeFallthroughs(PropellerProfWriter &PPWriter) {
  auto from = syms.begin(), e = syms.end();
  auto to = std::next(from);
  auto WFrom = cnts.begin(), WEnd = cnts.end();
  auto WTo = std::next(WFrom);
  uint64_t LastCnt;
  SymbolEntry *LastTo = nullptr;
  for (; to != e; ++from, ++to, ++WFrom, ++WTo) {
    if (LastTo) {
      vector<SymbolEntry *> FTs;
      if (!PPWriter.calculateFallthroughBBs(LastTo, (*from), FTs))
        return false;
      if (!FTs.empty()) {
        // Note, after this operation, from / to are still valid.
        syms.insert(from, FTs.begin(), FTs.end());
        cnts.insert(WFrom, FTs.size(), ((*WFrom + LastCnt) >> 1));
      }
    }
    LastTo = *to;
    LastCnt = *WTo;
  }
  return true;
}

ostream & operator << (ostream &out, const path &path) {
  out << "path [" << path.syms.size() << "]: ";
  auto i = path.syms.begin(), J = path.syms.end();
  auto p = path.cnts.begin();
  out << (*i)->ordinal;
  for (++i, ++p; i != J; ++i, ++p)
    out << "-(" << *p << ")->" << (*i)->ordinal;
  return out;
}

#endif
