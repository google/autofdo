#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_propeller_flags.h"
#include "llvm_propeller_profile_writer.h"
#include "llvm_propeller_profile_format.h"

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
  auto a = bbName.split(llvm::propeller::BASIC_BLOCK_SEPARATOR);
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

bool PropellerProfWriter::write() {
  if (!(initBinaryFile() && findBinaryBuildId() &&
        ((FLAGS_dot_number_encoding && populateSymbolMap2()) ||
          populateSymbolMap())))
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
  for (auto &p : symbolNameMap)
    for (auto &q : p.second)
      if (q.second->bbTag)
        ++totalBBsAll;
      else
        ++totalFuncs;
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
  SymbolEntry *lastFuncSymbol = nullptr;
  uint32_t bbCount = 0;
  auto startNewFunctionParagraph = [&fout, &bbCount,
                                    &lastFuncSymbol](SymbolEntry *fSymbol) {
    // If we haven't output any BB symbols for lastFuncSymbol, we then
    // output "!!0" which means the entry block is hot, before we switch
    // to processing another hot function.
    if (lastFuncSymbol && !bbCount) fout << "!!0" << std::endl;
    fout << '!' << SymNameF(fSymbol) << std::endl;
    lastFuncSymbol = fSymbol;
    bbCount = 0;
  };
  for (auto *se : hotSymbols)
    if (se->bbTag) {
      if (lastFuncSymbol != se->containingFunc)
        startNewFunctionParagraph(se->containingFunc);
      if (FLAGS_dot_number_encoding) {
        auto funcPartPos = se->name.find_first_of('.');
        StringRef bbPart = se->name.substr(funcPartPos + 1);
        fout << "!!" << bbPart.drop_back(1).str() << std::endl;
      } else
        fout << "!!" << se->name.size() << std::endl;
      ++bbCount;
    } else
      startNewFunctionParagraph(se);
}

void PropellerProfWriter::writeSymbols(ofstream &fout) {
  this->symbolsWritten = 0;
  uint64_t symbolOrdinal = 0;
  fout << "Symbols" << std::endl;
  for (auto &le : addrMap) {

    if (!FLAGS_dot_number_encoding) {
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
    }

    for (auto *sePtr : le.second) {
      SymbolEntry &se = *sePtr;
      fout << SymOrdinalF(se) << " " << SymSizeF(se) << " ";
      ++this->symbolsWritten;
      if (se.bbTag) {
        fout << SymOrdinalF(se.containingFunc) << ".";
        StringRef bbIndex = se.name;
        if (FLAGS_dot_number_encoding) {
          bbIndex = se.name.substr(se.name.find_first_of('.') + 1);
          if (se.bbTagType != SymbolEntry::BB_NORMAL)
            fout << bbIndex.str();
          else
            fout << bbIndex.drop_back(1).str();
        } else {
          fout << ::dec << (uint64_t)(bbIndex.size());
          if (bbIndex.front() != 'a') fout << bbIndex.front();
        }
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
        type = 'R';
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
               << " -> " << SymShortF(to) << "): " << ::dec << path.size()
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
  stringstream ss;
  ss << "Loadable and executable segments:\n";
  for (auto &seg : phdrLoadMap) {
    ss << "\tvaddr=" << hex0x << seg.first << ", memsz=" << hex0x << seg.second
       << std::endl;
  }
  LOG(INFO) << ss.str();
  return true;
}

bool PropellerProfWriter::initBinaryFile() {
  auto fileOrError = llvm::MemoryBuffer::getFile(binaryFileName);
  if (!fileOrError) {
    LOG(ERROR) << "Failed to read file '" << binaryFileName << "'.";
    return false;
  }
  this->binaryFileContent = std::move(*fileOrError);

  auto objOrError = llvm::object::ObjectFile::createELFObjectFile(
      llvm::MemoryBufferRef(*(this->binaryFileContent)));
  if (!objOrError) {
    LOG(ERROR) << "Not a valid ELF file '" << binaryFileName << "'.";
    return false;
  }
  this->objFile = std::move(*objOrError);

  auto *elfObjBase = dyn_cast<ELFObjectFileBase, ObjectFile>(objFile.get());
  binaryIsPIE = (elfObjBase->getEType() == llvm::ELF::ET_DYN);
  if (binaryIsPIE) {
    const char *elfIdent = binaryFileContent->getBufferStart();
    const char elfClass = elfIdent[4];
    const char elfData = elfIdent[5];
    if (elfClass == 1 && elfData == 1) {
      fillELFPhdr<llvm::object::ELF32LE>(elfObjBase, phdrLoadMap);
    } else if (elfClass == 1 && elfData == 2) {
      fillELFPhdr<llvm::object::ELF32BE>(elfObjBase, phdrLoadMap);
    } else if (elfClass == 2 && elfData == 1) {
      fillELFPhdr<llvm::object::ELF64LE>(elfObjBase, phdrLoadMap);
    } else if (elfClass == 2 && elfData == 2) {
      fillELFPhdr<llvm::object::ELF64BE>(elfObjBase, phdrLoadMap);
    } else {
      assert(false);
    }
  }
  LOG(INFO) << "'" << this->binaryFileName
            << "' is PIE binary: " << binaryIsPIE;
  return true;
}

bool PropellerProfWriter::populateSymbolMap2() {
  auto symbols = objFile->symbols();
  auto isBBSym = [](StringRef sname, uint8_t sbinding, StringRef &bbIndex) {
    const static char DIGITS[] = "0123456789";
    if (sbinding == llvm::ELF::STB_LOCAL) {
      size_t r = sname.find_first_not_of(DIGITS, 0);
      if (r != StringRef::npos && r > 0 && sname[r] == '.' &&
          r + 1 < sname.size() &&
          sname.find_first_not_of(DIGITS, r + 1) == sname.size() - 1 &&
          (sname.back() == 'a' || sname.back() == 'l' || sname.back() == 'r' ||
           sname.back() == 'L')) {
        bbIndex = sname.substr(r + 1);
        return true;
      }
    }
    return false;
  };

  auto getBBFuncPart = [](StringRef bbFullName) -> StringRef {
    return bbFullName.substr(0, bbFullName.find_first_of('.'));
  };

  auto getBBIndexPart = [](StringRef bbFullName) -> StringRef {
    return bbFullName.substr(bbFullName.find_first_of('.') + 1);
  };

  for (const auto &sym : symbols) {
    auto addrR = sym.getAddress();
    auto secR = sym.getSection();
    auto NameR = sym.getName();
    auto typeR = sym.getType();
    auto sectionR = sym.getSection();

    if (!(addrR && *addrR && secR && (*secR)->isText() && NameR && typeR &&
          sectionR))
      continue;

    StringRef name = *NameR;
    if (name.empty()) continue;
    uint64_t addr = *addrR;
    uint8_t type(*typeR);
    llvm::object::ELFSymbolRef elfSym(sym);
    uint64_t size = elfSym.getSize();
    uint8_t binding = elfSym.getBinding();
    uint64_t shndx = (*sectionR)->getIndex();

    StringRef bbIndex;
    bool isFunction = (type == llvm::object::SymbolRef::ST_Function);
    bool isBB = isBBSym(name, binding, bbIndex);
    if (!isFunction && !isBB) continue;

    auto &addrL = addrMap[addr];
    if (!addrL.empty()) {
      SymbolEntry *symbolIsAliasedWith = nullptr;
      for (auto *sym : addrL) {
        if (sym->size == size || isFunction && !sym->bbTag) {
          sym->aliases.push_back(name);
          symbolIsAliasedWith = sym;
          break;
        }
      }
      if (symbolIsAliasedWith) continue;
    }

    SymbolEntry *incompleteSymbol = *(addrL.insert(
        addrL.begin(), new SymbolEntry(0, name, SymbolEntry::AliasesTy(), addr,
                                       size, isBB, nullptr)));
    incompleteSymbol->bbTagType =
        (isBB ? SymbolEntry::toBBTagType(bbIndex.back())
              : SymbolEntry::BB_NONE);
  }

  auto emplaceSymbol = [this, &getBBIndexPart](SymbolEntry *se) -> bool {
    if (!symbolNameMap[se->containingFunc->name]
             .emplace((se->bbTag ? getBBIndexPart(se->name) : StringRef("")),
                      se)
             .second) {
      LOG(ERROR) << "Found symbols with duplicated name: '"
                 << se->containingFunc->name.str()
                 << (se->bbTag ? std::string(".") + se->name.str()
                               : std::string(""))
                 << "'.";

      return false;
    }
    return true;
  };

  uint64_t symbolOrdinal = 0;
  auto processSameAddressBBs = [&symbolOrdinal, &emplaceSymbol](
                                   uint64_t addr, list<SymbolEntry *> &bbs,
                                   SymbolEntry *containingFuc) -> bool {
    if (bbs.size() > 2) {
      LOG(ERROR) << "Analyzing failure: more than 2 (>2) bbs defined on the "
                    "same address "
                 << hex0x << addr;
      return false;
    }
    if (bbs.size() > 1) {
      bbs.sort([](SymbolEntry *s1, SymbolEntry *s2) { return s1->size == 0; });
      if (bbs.front()->size != 0 && bbs.back()->size != 0) {
        LOG(ERROR) << "Analyzing failure: at address " << hex0x << addr
                   << ", there are 2 non-empty bbs.";
        return false;
      }
    }
    for (auto *se : bbs) {
      assert(se->bbTag);
      se->ordinal = ++symbolOrdinal;
      se->containingFunc = containingFuc;
      // recordFuncIndex(se);
      emplaceSymbol(se);
    }
    return true;
  };

  auto sortByOrdinal = [](list<SymbolEntry *> &sl) {
    sl.sort([](SymbolEntry *s1, SymbolEntry *s2) {
      return s1->ordinal < s2->ordinal;
    });
  };

  SymbolEntry *currentFunc = nullptr;
  for (auto p = addrMap.begin(); p != addrMap.end(); ++p) {
    uint64_t addr = p->first;
    auto &symL = p->second;
    if (symL.empty()) continue;
    if (symL.size() == 1) {
      SymbolEntry *se = symL.front();
      if (se->bbTag) {
        if (currentFunc) {
          se->containingFunc = currentFunc;
        } else {
          LOG(ERROR) << "Analyzing failure: at address " << hex0x << addr
                     << ", symbol '" << se->name.str()
                     << "' does not have a containing function.";
          return false;
        }
      } else {
        se->containingFunc = se;
        currentFunc = se;
      }
      if (!emplaceSymbol(se)) return false;
      se->ordinal = ++symbolOrdinal;
      // recordFuncIndex(se);
      continue;
    }

    // We have multiple symbols at address.
    bool allAreBBs = true;
    SymbolEntry *funcSym = nullptr;
    for (SymbolEntry *s : symL) {
      allAreBBs &= s->bbTag;
      if (!s->bbTag) {
        if (funcSym) {
          LOG(ERROR) << "Analyzing failure: at address " << hex0x << addr
                     << ", more than 1 function symbols defined.";
          return false;
        }
        funcSym = s;
      }
    }
    if (allAreBBs) {
      if (!currentFunc) {
        LOG(ERROR) << "Analyzing failure: at address " << hex0x << addr
                   << ", symbols do not have a containing func.";
        return false;
      }
      processSameAddressBBs(addr, symL, currentFunc);
      sortByOrdinal(symL);
      continue;
    }

    if (symL.size() == 2) {
      SymbolEntry *s1 = symL.front(), *s2 = symL.back();
      if (s1 != funcSym && s2 != funcSym) {
        LOG(ERROR) << "Analyzing failure: internal error a at address " << hex0x
                   << addr;
        return false;
      }
      SymbolEntry *bbSym = s1 == funcSym ? s2 : s1;
      if (!bbSym->bbTag) {
        LOG(ERROR) << "Analyzing failure: unexpected non-bb symbol at " << hex0x
                   << addr;
        return false;
      }
      // So now we have funcSym and bbSym on the same address.
      // Case 1:
      // 00000000018a3047 000000000000000c t 99.6r
      // 00000000018a3060 000000000000000c t 100.1a
      // 00000000018a3060 000000000000000d W _ZN4llvm1
      SymbolEntry *lastSym = nullptr;
      if (p != addrMap.begin())
        lastSym = std::prev(p)->second.back();
      symL.clear();
      if (lastSym && lastSym->bbTag &&
          getBBFuncPart(lastSym->name) == getBBFuncPart(bbSym->name)) {
        bbSym->containingFunc = lastSym->containingFunc;
        symL.push_back(bbSym);
        symL.push_back(funcSym);
      } else {
        bbSym->containingFunc = funcSym;
        symL.push_back(funcSym);
        symL.push_back(bbSym);
      }
      funcSym->containingFunc = funcSym;
      symL.front()->ordinal = ++symbolOrdinal;
      symL.back()->ordinal = ++symbolOrdinal;
      currentFunc = funcSym;
    } else {
      LOG(ERROR) << "Analyzing failure: multiple func/bb symbol mixed on the "
                    "same address "
                 << hex0x << addr;
      return false;
    }
  }  // end of iterating all address.
  return true;
}

bool PropellerProfWriter::populateSymbolMap() {
  auto symbols = objFile->symbols();
  const set<StringRef> excludedSymbols{"__cxx_global_array_dtor"};
  for (const auto &sym : symbols) {
    auto addrR = sym.getAddress();
    auto secR = sym.getSection();
    auto NameR = sym.getName();
    auto typeR = sym.getType();

    if (!(addrR && *addrR && secR && (*secR)->isText() && NameR && typeR))
      continue;

    StringRef name = *NameR;
    if (name.empty()) continue;
    uint64_t addr = *addrR;
    uint8_t type(*typeR);
    llvm::object::ELFSymbolRef ELFSym(sym);
    uint64_t size = ELFSym.getSize();

    StringRef bbFunctionName;
    bool isFunction = (type == llvm::object::SymbolRef::ST_Function);
    bool isBB = SymbolEntry::isBBSymbol(name, &bbFunctionName);

    if (!isFunction && !isBB) continue;
    if (excludedSymbols.find(isBB ? bbFunctionName : name) !=
        excludedSymbols.end()) {
      continue;
    }

    auto &addrL = addrMap[addr];
    if (!addrL.empty()) {
      // If we already have a symbol at the same address, merge
      // them together.
      SymbolEntry *symbolIsAliasedWith = nullptr;
      for (auto *sym : addrL) {
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
          symbolIsAliasedWith = sym;
          break;
        }
      }
      if (symbolIsAliasedWith) continue;
    }

    // Delete symbol with same name from symbolNameMap and addrMap.
    auto existingNameR = symbolNameMap.find(name);
    if (existingNameR != symbolNameMap.end()) {
      LOG(INFO) << "Dropped duplicate symbol \"" << SymNameF(name) << "\". "
                << "Consider using \"-funique-internal-funcnames\" to "
                   "dedupe internal function names.";
      map<uint64_t, list<SymbolEntry *>>::iterator existingLI =
          addrMap.find(existingNameR->second[name].get()->addr);
      if (existingLI != addrMap.end()) {
        existingLI->second.remove_if(
            [&name](SymbolEntry *sym) { return sym->name == name; });
      }
      symbolNameMap.erase(existingNameR);
      continue;
    }

    SymbolEntry *newSymbolEntry = *(addrL.insert(
        addrL.begin(), new SymbolEntry(0, name, SymbolEntry::AliasesTy(), addr,
                                       size, SymbolEntry::isBBSymbol(name))));
    // Set the BB Tag type according to the first character of the symbol name.
    if (newSymbolEntry->bbTag)
      newSymbolEntry->bbTagType = SymbolEntry::toBBTagType(name.front());
    else {
      newSymbolEntry->bbTagType = SymbolEntry::BB_NONE;
      newSymbolEntry->containingFunc = newSymbolEntry;
    }
    symbolNameMap[name].emplace(std::piecewise_construct,
                                std::forward_as_tuple(name),
                                std::forward_as_tuple(newSymbolEntry));
  }  // End of iterating all symbols.

  // Now scan all the symbols in address order to create function <-> bb
  // relationship.
  uint64_t bbSymbolDropped = 0;
  decltype(addrMap)::iterator lastFuncPos = addrMap.end();
  for (auto p = addrMap.begin(), q = addrMap.end(); p != q; ++p) {
    int funcCount = 0;
    for (auto *sym : p->second) {
      if (sym->isFunction()) {
        if (++funcCount > 1) {
          // 2 different functions start at the same address, but with different
          // sizes, this is not supported.
          LOG(ERROR)
              << "Analyzing failure: at address " << hex0x << p->first
              << ", there are more than 1 functions that have different sizes.";
          return false;
        }
        lastFuncPos = p;
      }
    }

    if (lastFuncPos == addrMap.end()) continue;
    for (auto *sym : p->second) {
      if (!sym->bbTag) {
        // Set a function's wrapping function to itself.
        sym->containingFunc = sym;
        continue;
      }
      // This is a bb symbol, find a wrapping func for it.
      SymbolEntry *containingFunc = nullptr;
      for (SymbolEntry *fp : lastFuncPos->second) {
        if (fp->isFunction() && !fp->bbTag && containsAnotherSymbol(fp,sym) &&
            isFunctionForBBName(fp, sym->name)) {
          if (containingFunc == nullptr) {
            containingFunc = fp;
          } else {
            // Already has a containing function, so we have at least 2
            // different functions with different sizes but start at the same
            // address, impossible?
            LOG(ERROR) << "Analyzing failure: at address " << hex0x
                       << lastFuncPos->first
                       << ", there are 2 different functions: "
                       << SymNameF(containingFunc) << " and "
                       << SymNameF(fp);
            return false;
          }
        }
      }
      if (!containingFunc) {
        // Disambiguate the following case:
        // 0x10 foo       size = 2
        // 0x12 foo.bb.1  size = 2
        // 0x14 foo.bb.2  size = 0
        // 0x14 bar  <- lastFuncPos set is to bar.
        // 0x14 bar.bb.1
        // In this scenario, we seek lower address.
        auto tempI = lastFuncPos;
        int functionSymbolSeen = 0;
        while (tempI != addrMap.begin()) {
          tempI = std::prev(tempI);
          bool isFunction = false;
          for (auto *KS : tempI->second) {
            isFunction |= KS->isFunction();
            if (KS->isFunction() && !KS->bbTag &&
                containsAnotherSymbol(KS, sym) &&
                isFunctionForBBName(KS, sym->name)) {
              containingFunc = KS;
              break;
            }
          }
          functionSymbolSeen += isFunction ? 1 : 0;
          // Only go back for at most 2 function symbols.
          if (functionSymbolSeen > 2) break;
        }
      }
      sym->containingFunc = containingFunc;
      if (sym->containingFunc == nullptr) {
        LOG(ERROR) << "Dropped bb symbol without any wrapping function: \""
                   << SymShortF(sym) << "\"";
        ++bbSymbolDropped;
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
        auto a = sym->name.split(llvm::propeller::BASIC_BLOCK_SEPARATOR);
        auto expectFuncName = a.second;
        auto &aliases = containingFunc->aliases;
        if (expectFuncName != containingFunc->name) {
          SymbolEntry::AliasesTy::iterator p, q;
          for (p = aliases.begin(), q = aliases.end(); p != q; ++p)
            if (*p == expectFuncName) break;

          if (p == q) {
            LOG(ERROR) << "Internal check error: bb symbol '" << sym->name.str()
                       << "' does not have a valid wrapping function.";
            return false;
          }
          StringRef oldName = containingFunc->name;
          containingFunc->name = *p;
          aliases.erase(p);
          aliases.push_back(oldName);
        }
      }

      // Replace the whole name (e.g. "aaaa.BB.foo" with "aaaa" only);
      StringRef fName, bName;
      bool r = SymbolEntry::isBBSymbol(sym->name, &fName, &bName);
      (void)(r);
      assert(r);
      if (fName != sym->containingFunc->name) {
        LOG(ERROR) << "Internal check error: bb symbol '" << sym->name.str()
                       << "' does not have a valid wrapping function.";
      }
      sym->name = bName;
    }  // End of iterating p->second
  }    // End of iterating addrMap.
  if (bbSymbolDropped)
    LOG(INFO) << "Dropped " << ::dec << CommaF(bbSymbolDropped)
              << " bb symbol(s).";
  return true;
}

bool PropellerProfWriter::parsePerfData() {
  this->perfDataFileParsed = 0;
  StringRef fn(perfFileName);
  while (!fn.empty()) {
    StringRef perfName;
    std::tie(perfName, fn) = fn.split(',');
    if (!parsePerfData(perfName.str())) {
      return false;
    }
    ++perfDataFileParsed;
  }
  LOG(INFO) << "Processed " << perfDataFileParsed << " perf file(s).";
  return true;
}

bool PropellerProfWriter::parsePerfData(const string &pName) {
  quipper::PerfReader perfReader;
  if (!perfReader.ReadFile(pName)) {
    LOG(ERROR) << "Failed to read perf data file: " << pName;
    return false;
  }

  quipper::PerfParser parser(&perfReader);
  if (!parser.ParseRawEvents()) {
    LOG(ERROR) << "Failed to parse perf raw events for perf file: '" << pName
               << "'.";
    return false;
  }

  if (!FLAGS_ignore_build_id) {
    if (!setupBinaryMMapName(perfReader, pName)) {
      return false;
    }
  }

  if (!setupMMaps(parser, pName)) {
    LOG(ERROR) << "Failed to find perf mmaps for binary '" << binaryFileName
               << "'.";
    return false;
  }

  return aggregateLBR(parser);
}

bool PropellerProfWriter::setupMMaps(quipper::PerfParser &parser,
                                     const string &pName) {
  // Depends on the binary file name, if
  //   - it is absolute, compares it agains the full path
  //   - it is relative, only compares the file name part
  // Note: compFunc is constructed in a way so that there is no branch /
  // conditional test inside the function.
  struct BinaryNameComparator {
    BinaryNameComparator(const string &binaryFileName) {
      if (llvm::sys::path::is_absolute(binaryFileName)) {
        comparePart = StringRef(binaryFileName);
        pathChanger = NullPathChanger;
      } else {
        comparePart = llvm::sys::path::filename(binaryFileName);
        pathChanger = NameOnlyPathChanger;
      }
    }

    bool operator()(const string &path) {
      return comparePart == pathChanger(path);
    }

    StringRef comparePart;
    std::function<StringRef(const string &)> pathChanger;

    static StringRef NullPathChanger(const string &sym) {
      return StringRef(sym);
    }
    static StringRef NameOnlyPathChanger(const string &sym) {
      return llvm::sys::path::filename(StringRef(sym));
    }
  } compFunc(FLAGS_match_mmap_file.empty()
                 ? (this->binaryMMapName.empty() ? binaryFileName
                                                 : this->binaryMMapName)
                 : FLAGS_match_mmap_file);

  for (const auto &pe : parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *eventPtr = pe.event_ptr;
    if (eventPtr->event_type_case() !=
        quipper::PerfDataProto_PerfEvent::kMmapEvent)
      continue;

    const quipper::PerfDataProto_MMapEvent &mmap = eventPtr->mmap_event();
    if (!mmap.has_filename()) continue;

    const string &MMapFileName = mmap.filename();
    if (!compFunc(MMapFileName) || !mmap.has_start() || !mmap.has_len() ||
        !mmap.has_pid())
      continue;

    if (this->binaryMMapName.empty()) {
      this->binaryMMapName = MMapFileName;
    } else if (binaryMMapName != MMapFileName) {
      LOG(ERROR) << "'" << binaryFileName
                 << "' is not specific enough. It matches both '"
                 << binaryMMapName << "' and '" << MMapFileName
                 << "' in the perf data file '" << pName
                 << "'. Consider using absolute file name.";
      return false;
    }
    uint64_t loadAddr = mmap.start();
    uint64_t loadSize = mmap.len();
    uint64_t pageOffset = mmap.has_pgoff() ? mmap.pgoff() : 0;

    // For the same binary, mmap can only be different if it is a PIE binary. So
    // for non-PIE binaries, we check all MMaps are equal and merge them into
    // binaryMMapByPid[0].
    uint64_t mPid = binaryIsPIE ? mmap.pid() : 0;
    set<MMapEntry> &loadMap = binaryMMapByPid[mPid];
    // Check for mmap conflicts.
    if (!checkBinaryMMapConflictionAndEmplace(loadAddr, loadSize, pageOffset,
                                              loadMap)) {
      stringstream ss;
      ss << "Found conflict mmap event: "
         << MMapEntry{loadAddr, loadSize, pageOffset}
         << ". Existing mmap entries: " << std::endl;
      for (auto &me : loadMap) {
        ss << "\t" << me << std::endl;
      }
      LOG(ERROR) << ss.str();
      return false;
    }
  }  // End of iterating mmap events.

  if (!std::accumulate(
          binaryMMapByPid.begin(), binaryMMapByPid.end(), 0,
          [](uint64_t V, const decltype(binaryMMapByPid)::value_type &sym)
              -> uint64_t { return V + sym.second.size(); })) {
    LOG(ERROR) << "Failed to find mmap entries in '" << pName << "' for '"
               << binaryFileName << "'.";
    return false;
  }
  for (auto &mpid : binaryMMapByPid) {
    stringstream ss;
    ss << "Found mmap in '" << pName << "' for binary: '" << binaryFileName
       << "', pid=" << ::dec << mpid.first << " (0 for non-pie executables)"
       << std::endl;
    for (auto &mm : mpid.second) {
      ss << "\t" << mm << std::endl;
    }
    LOG(INFO) << ss.str();
  }
  return true;
}

bool PropellerProfWriter::checkBinaryMMapConflictionAndEmplace(
    uint64_t loadAddr, uint64_t loadSize, uint64_t pageOffset,
    set<MMapEntry> &mm) {
  for (const MMapEntry &e : mm) {
    if (e.loadAddr == loadAddr && e.loadSize == loadSize &&
        e.pageOffset == pageOffset)
      return true;
    if (!((loadAddr + loadSize <= e.loadAddr) ||
          (e.loadAddr + e.loadSize <= loadAddr)))
      return false;
  }
  auto r = mm.emplace(loadAddr, loadSize, pageOffset);
  assert(r.second);
  return true;
}

bool PropellerProfWriter::setupBinaryMMapName(quipper::PerfReader &perfReader,
                                              const string &pName) {
  this->binaryMMapName = "";
  if (FLAGS_ignore_build_id || this->binaryBuildId.empty()) {
    return true;
  }
  list<pair<string, string>> existingBuildIds;
  for (const auto &buildId : perfReader.build_ids()) {
    if (buildId.has_filename() && buildId.has_build_id_hash()) {
      string PerfBuildId = buildId.build_id_hash();
      quipper::PerfizeBuildIDString(&PerfBuildId);
      existingBuildIds.emplace_back(buildId.filename(), PerfBuildId);
      if (PerfBuildId == this->binaryBuildId) {
        this->binaryMMapName = buildId.filename();
        LOG(INFO) << "Found file with matching buildId in perf file '" << pName
                  << "': " << this->binaryMMapName;
        return true;
      }
    }
  }
  stringstream ss;
  ss << "No file with matching buildId in perf data '" << pName
     << "', which contains the following <file, buildid>:" << std::endl;
  for (auto &p : existingBuildIds) {
    ss << "\t" << p.first << ": " << BuildIdWrapper(p.second.c_str())
       << std::endl;
  }
  LOG(INFO) << ss.str();
  return false;
}

bool PropellerProfWriter::aggregateLBR(quipper::PerfParser &parser) {
  uint64_t brStackCount = 0;
  for (const auto &pe : parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *eventPtr = pe.event_ptr;
    if (eventPtr->event_type_case() !=
        quipper::PerfDataProto_PerfEvent::kSampleEvent)
      continue;

    auto &sEvent = eventPtr->sample_event();
    if (!sEvent.has_pid()) continue;
    auto brStack = sEvent.branch_stack();
    if (brStack.empty()) continue;
    uint64_t pid = binaryIsPIE ? sEvent.pid() : 0;
    if (binaryMMapByPid.find(pid) == binaryMMapByPid.end()) continue;
    auto &branchCounters = branchCountersByPid[pid];
    auto &fallthroughCounters = fallthroughCountersByPid[pid];
    uint64_t lastFrom = INVALID_ADDRESS;
    uint64_t lastTo = INVALID_ADDRESS;
    brStackCount += brStack.size();
    std::vector<SymbolEntry *> symSeq{};
    SymbolEntry *LastToSym = nullptr;
    for (int p = brStack.size() - 1; p >= 0; --p) {
      const auto &be = brStack.Get(p);
      uint64_t from = be.from_ip();
      uint64_t to = be.to_ip();
      // TODO: LBR sometimes duplicates the first entry by mistake. For now we
      // treat these to be true entries.

      // if (p == 0 && from == lastFrom && to == lastTo) {
      //  LOG(INFO) << "Ignoring duplicate LBR entry: 0x" << std::hex <<
      //             from
      //             << "-> 0x" << to << std::dec << "\n";
      //  continue;
      //}

      ++(branchCounters[make_pair(from, to)]);
      if (lastTo != INVALID_ADDRESS && lastTo <= from)
        ++(fallthroughCounters[make_pair(lastTo, from)]);

      // Aggregate path profile information.
      if (FLAGS_gen_path_profile) {
        SymbolEntry *fromSym, *toSym;
        if (lastTo != INVALID_ADDRESS && lastTo > from)
          goto save_path;

        fromSym = findSymbolAtAddress(pid, from);
        toSym = findSymbolAtAddress(pid, to);

        if (p == 0 && from == lastFrom && to == lastTo) {
          // LOG(INFO) << "Ignoring duplicate LBR entry: 0x" << std::hex <<
          // from
          //           << "-> 0x" << to << std::dec << "\n";
          goto done_path;
        }

        // if (fromSym && toSym)
        //   std::cout << "pair is: " << SymNameF(fromSym)
        //             << " -> " << SymNameF(toSym) << std::endl;

        if (fromSym && (symSeq.empty() || symSeq.back()->containingFunc ==
                                              fromSym->containingFunc)) {
          // If we have 1->2 2->3, we only record 1->2->3.
          if (symSeq.empty() || symSeq.back() != fromSym)
            symSeq.push_back(fromSym);

          if (/*from < to && */toSym &&
              toSym->containingFunc == fromSym->containingFunc) {
            symSeq.push_back(toSym);
            goto done_path;
          }
          // else fallthrough to save_path.
        }
      save_path:
        pathProfile.addSymSeq(symSeq);
        symSeq.clear();
      done_path:;
      }  // Done path profile

      lastTo = to;
      lastFrom = from;
    }  // end of iterating one br record
    if (FLAGS_gen_path_profile) {
      pathProfile.addSymSeq(symSeq);
      symSeq.clear();
    }
  }  // End of iterating all br records.
  if (brStackCount < 100) {
    LOG(ERROR) << "Too few brstack records (only " << brStackCount
               << " record(s) found), cannot continue.";
    return false;
  }
  LOG(INFO) << "Processed " << CommaF(brStackCount) << " lbr records.";
  if (FLAGS_gen_path_profile)
    pathProfile.printPaths(std::cout, *this);
  return true;
}

bool PropellerProfWriter::findBinaryBuildId() {
  this->binaryBuildId = "";
  if (FLAGS_ignore_build_id) return true;
  bool buildIdFound = false;
  for (auto &sr : objFile->sections()) {
    llvm::object::ELFSectionRef esr(sr);
    StringRef sName;
    auto exSecRefName = sr.getName();
    if (exSecRefName) {
      sName = *exSecRefName;
    } else {
      continue;
    }
    auto expectedSContents = sr.getContents();
    if (esr.getType() == llvm::ELF::SHT_NOTE && sName == ".note.gnu.build-id" &&
        expectedSContents && !expectedSContents->empty()) {
      StringRef sContents = *expectedSContents;
      const unsigned char *p = sContents.bytes_begin() + 0x10;
      if (p >= sContents.bytes_end()) {
        LOG(INFO) << "Section '.note.gnu.build-id' does not contain valid "
                     "build id information.";
        return true;
      }
      string buildId((const char *)p, sContents.size() - 0x10);
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

#endif
