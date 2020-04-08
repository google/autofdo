#include "config.h"
#if defined(HAVE_LLVM)
#ifndef _LLVM_PLO_PROFILE_WRITER_H_
#define _LLVM_PLO_PROFILE_WRITER_H_

#include "llvm_propeller_path_profile.h"

#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <ostream>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ProfileData/BBSectionsProf.h"
#include "llvm/Support/MemoryBuffer.h"
#include "CodeLayout/PropellerCFG.h"
#include "CodeLayout/CodeLayout.h"

using std::list;
using std::map;
using std::multiset;
using std::ostream;
using std::ofstream;
using std::pair;
using std::priority_queue;
using std::set;
using std::string;
using std::pair;
using std::unique_ptr;
using std::vector;

using llvm::propeller::SymbolEntry;
using llvm::SmallVector;
using llvm::StringRef;

using llvm::propeller::ControlFlowGraph;
using llvm::propeller::CFGNode;

namespace quipper {
class PerfParser;
class PerfReader;
}  // namespace quipper

// This class, given binary and perf.data file paths, writes profile data
// that are to be consumed by plo optimizer.

// a sample output is like below:
//
// symbols
// 1 0 N.init/_init
// 2 0 N.plt
// 3 0 N.plt.got
// 4 0 N.text
// 5 2b N_start
// 6 0 Nderegister_tm_clones
// 7 0 Nregister_tm_clones
// 8 0 N__do_global_dtors_aux
// 9 0 Nframe_dummy
// 10 2c Ncompute_flag
// 11 7c Nmain
// 12 f 11.1
// 13 28 11.2
// 14 b 11.3
// 15 a 11.4
// 16 65 N__libc_csu_init
// 17 2 N__libc_csu_fini
// 18 0 N.fini/_fini
// 19 5e N_ZN9assistantD2Ev/_ZN9assistantD1Ev
// Branches
// 10 12 232590 r
// 12 10 234842 c
// 12 14 143608
// 14 12 227040
// Fallthroughs
// 10 10 225131
// 10 12 2255
// 12 10 2283
// 12 12 362886
// 12 14 77103
// 14 12 1376
// 14 14 140856
// !func1
// !func2
// !func3
//
// The file consists of 4 parts, "symbols", "Branches", "Fallthroughs" and
// Funclist.
//
// Funclist contains lines that starts with "!", and everything following that
// will be the function name that's to be consumed by compiler (for bb section
// generation purpose).
//
// Each line in "symbols" section contains the following field:
//   index    - in decimal, unique for each symbol, start from 1
//   size     - in hex, without "0x"
//   name     - either starts with "N" or a digit. In the former case,
//              everything after N is the symbol name. In the latter case, it's
//              in the form of "a.b", "a" is a symbol index, "b" is the bb
//              identification string (could be an index number). For the above
//              example, name "14.2" means "main.bb.2", because "14" points to
//              symbol main. Also note, symbols could have aliases, in such
//              case, aliases are concatenated with the original name with a
//              '/'. For example, symbol 17391 contains 2 aliases.
// Note, the symbols listed are in strict non-decreasing address order.
//
// Each line in "Branches" section contains the following field:
//   from     - sym_index, in decimal
//   to       - sym_index, in decimal
//   cnt      - counter, in decimal
//   c/r      - a field indicate whether this is a function call or a return,
//              could be empty if it's just a normal branch.
//
// Each line in "Fallthroughs" section contains exactly the same fields as in
// "Branches" section, except the "c" field.
//
// Each line that starts with "!" is followed by a function name, for
// which, bb sections will be generated.

class MMapEntry {
 public:
  MMapEntry(uint64_t addr, uint64_t size, uint64_t pageOff)
      : loadAddr(addr), loadSize(size), pageOffset(pageOff) {}
  ~MMapEntry() {}

  uint64_t loadAddr;
  uint64_t loadSize;
  uint64_t pageOffset;

  uint64_t getEndAddr() const { return loadAddr + loadSize; }

  bool operator<(const MMapEntry &mm) const {
    return this->loadAddr < mm.loadAddr;
  }
  
};

class PropellerProfWriter {
 public:
  PropellerProfWriter(const string &bfn, const string &pfn, const string &ofn, const string &sofn);
  ~PropellerProfWriter();

  bool write();

 private:
  const string binaryFileName;
  const string perfFileName;
  const string propOutFileName;
  const string symOrderFileName;

  // binaryFileContent must be the last one to be destroyed.
  // So it appears first in this section.
  unique_ptr<llvm::MemoryBuffer> binaryFileContent;
  unique_ptr<llvm::object::ObjectFile> objFile;
  // All symbol handlers.
  map<StringRef, map<StringRef, unique_ptr<SymbolEntry>>> symbolNameMap;
  map<SymbolEntry *, std::vector<SymbolEntry*>> symbolEntryMap;
  map<SymbolEntry *, std::unique_ptr<ControlFlowGraph>> cfgs;
  map<SymbolEntry *, CFGNode *> symbolNodeMap;
  // Symbol start address -> Symbol list.
  map<uint64_t, list<SymbolEntry *>> addrMap;
  using CounterTy = map<pair<uint64_t, uint64_t>, uint64_t>;
  map<uint64_t, CounterTy> branchCountersByPid;
  map<uint64_t, CounterTy> fallthroughCountersByPid;
  map<uint64_t, uint64_t> phdrLoadMap;  // Only used when binary is PIE.

  // Instead of sorting "SymbolEntry *" by pointer address, we sort it by it's
  // symbol address and symbol ordinal, so we get a stable sort.
  struct SymbolEntryPairComp {
    using KeyT = pair<SymbolEntry *, SymbolEntry *>;
    bool operator()(const KeyT &k1, const KeyT &k2) const {
      if (k1.first->addr != k2.first->addr)
        return k1.first->addr < k2.first->addr;
      if (k1.second->addr != k2.second->addr)
        return k1.second->addr < k2.second->addr;
      return k1.first->ordinal == k2.first->ordinal
                 ? k1.second->ordinal < k2.second->ordinal
                 : k1.first->ordinal < k2.first->ordinal;
    }
  };

  map<pair<SymbolEntry *, SymbolEntry *>, uint64_t, SymbolEntryPairComp>
      fallthroughCountersBySymbol;

  // Group all bb symbols under their wrapping functions, and order function
  // groups by names.
  struct SymGroupComparator {
    bool operator()(SymbolEntry *s1, SymbolEntry *s2) const {
      if (s1->containingFunc->name != s2->containingFunc->name)
        return s1->containingFunc->name < s2->containingFunc->name;
      return s1->ordinal < s2->ordinal;
    }
  };
  set<SymbolEntry *, SymGroupComparator> hotSymbols;

  // Whether it is Position Independent Executable. If so, addresses from perf
  // file must be adjusted to map to symbols.
  bool binaryIsPIE;
  // mmap entries, pid -> BinaryLoadMap
  map<uint64_t, set<MMapEntry>> binaryMMapByPid;
  // All binary mmaps must have the same binaryMMapName.
  string binaryMMapName;
  // Nullptr if build id does not exist for binaryMMapName.
  string binaryBuildId;
  int32_t perfDataFileParsed;
  uint64_t symbolsWritten;
  uint64_t branchesWritten;
  uint64_t fallthroughsWritten;
  uint64_t extraBBsIncludedInFallthroughs;
  uint64_t totalCounters;
  uint64_t countersNotAddressed;
  uint64_t crossFunctionCounters;
  uint64_t fallthroughStartEndInDifferentFuncs;
  uint64_t fallthroughCalculationNumber;
  map<uint64_t, uint64_t> funcBBCounter;  // How many bb for each func.
  bool findBinaryBuildId();
  bool setupMMaps(quipper::PerfParser &parser, const string &pName);
  bool setupBinaryMMapName(quipper::PerfReader &reader, const string &pName);
  bool checkBinaryMMapConflictionAndEmplace(uint64_t loadAddr,
                                            uint64_t loadSize,
                                            uint64_t pageOffset,
                                            set<MMapEntry> &mmapEntry);

  uint64_t adjustAddressForPIE(uint64_t pid, uint64_t addr) const {
    auto i = binaryMMapByPid.find(pid);
    if (i == binaryMMapByPid.end()) return INVALID_ADDRESS;
    const MMapEntry *mmap = nullptr;
    for (const MMapEntry &p : i->second)
      if (p.loadAddr <= addr && addr < p.loadAddr + p.loadSize) mmap = &p;
    if (!mmap) {
      // fprintf(stderr, "!!! %ld 0x%lx\n", pid, addr);
      return INVALID_ADDRESS;
    }
    if (binaryIsPIE) {
      return addr - mmap->loadAddr + mmap->pageOffset;
    }
    return addr;
  }

  bool aggregateLBR(quipper::PerfParser &parser);
  bool calculateFallthroughBBs(SymbolEntry *from, SymbolEntry *to,
                               vector<SymbolEntry *> &result);

  bool initBinaryFile();
  bool populateSymbolMap();
  bool populateSymbolMap2();
  bool parsePerfData();
  bool parsePerfData(const string &pName);
  void writeOuts(ofstream &fout);
  void writeSymbols(ofstream &fout);
  void writeBranches(ofstream &fout);
  void writeFallthroughs(ofstream &fout);
  void writeHotFuncAndBBList(ofstream &fout);
  bool reorderSections(int64_t partBegin, int64_t partEnd, int64_t partNew);
  void summarize();

  SymbolEntry *findSymbolAtAddress(uint64_t pid, uint64_t addr);

  PathProfile pathProfile;

  friend PathProfile;
  friend Path;

  static const uint64_t INVALID_ADDRESS = uint64_t(-1);
};

#endif
#endif
