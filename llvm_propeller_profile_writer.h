#include "config.h"
#if defined(HAVE_LLVM)
#ifndef _LLVM_PLO_PROFILE_WRITER_H_
#define _LLVM_PLO_PROFILE_WRITER_H_

#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "lld/Common/PropellerCommon.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

using std::list;
using std::map;
using std::ofstream;
using std::pair;
using std::set;
using std::string;
using std::unique_ptr;

using lld::propeller::SymbolEntry;
using llvm::SmallVector;
using llvm::StringRef;

namespace quipper {
class PerfParser;
class PerfReader;
}

// This class, given binary and perf.data file paths, writes profile data
// that are to be consumed by plo optimizer.

// A sample output is like below:
//
// !func1
// !func2
// !func3
// Symbols
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
// 10 12 232590 R
// 12 10 234842 C
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
//
// The file consists of 4 parts, "Symbols", "Branches", "Fallthroughs" and
// Funclist.
//
// Funclist contains lines that starts with "!", and everything following that
// will be the function name that's to be consumed by compiler (for bb section
// generation purpose).
//
// Each line in "Symbols" section contains the following field:
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
//   C/R      - a field indicate whether this is a function call or a return,
//              could be empty if it's just a normal branch.
//
// Each line in "Fallthroughs" section contains exactly the same fields as in
// "Branches" section, except the "C" field.

class MMapEntry {
  public:
    MMapEntry(uint64_t Addr, uint64_t Size, uint64_t PgOff)
        : LoadAddr(Addr), LoadSize(Size), PageOffset(PgOff) {}
    ~MMapEntry() {}

    uint64_t LoadAddr;
    uint64_t LoadSize;
    uint64_t PageOffset;

    uint64_t getEndAddr() const { return LoadAddr + LoadSize; }

    bool operator < (const MMapEntry &M) const {
      return this->LoadAddr < M.LoadAddr;
    }
};

class PropellerProfWriter {
public:
  
  PropellerProfWriter(const string &BFN, const string &PFN, const string &OFN);
  ~PropellerProfWriter();

  bool write();

 private:
  const string BinaryFileName;
  const string PerfFileName;
  const string PropOutFileName;

  // BinaryFileContent must be the last one to be destroyed.
  // So it appears first in this section.
  unique_ptr<llvm::MemoryBuffer>       BinaryFileContent;
  unique_ptr<llvm::object::ObjectFile> ObjFile;
  // All symbol handlers.
  map<StringRef, unique_ptr<SymbolEntry>> SymbolNameMap;
  // Symbol start address -> Symbol list.
  map<uint64_t, list<SymbolEntry *>> AddrMap;
  using CounterTy = map<pair<uint64_t, uint64_t>, uint64_t>;
  map<uint64_t, CounterTy> BranchCountersByPid;
  map<uint64_t, CounterTy> FallthroughCountersByPid;
  map<uint64_t, uint64_t>  PhdrLoadMap;  // Only used when binary is PIE.

  // Instead of sorting "SymbolEntry *" by pointer address, we sort it by it's
  // symbol address and symbol ordinal, so we get a stable sort.
  struct SymbolEntryPairComp {
    using KeyT = pair<SymbolEntry *, SymbolEntry *>;
    bool operator()(const KeyT &K1, const KeyT &K2) const {
      if (K1.first->Addr != K2.first->Addr)
        return K1.first->Addr < K2.first->Addr;
      if (K1.second->Addr != K2.second->Addr)
        return K1.second->Addr < K2.second->Addr;
      return K1.first->Ordinal == K2.first->Ordinal
                 ? K1.second->Ordinal < K2.second->Ordinal
                 : K1.first->Ordinal < K2.first->Ordinal;
    }
  };

  map<pair<SymbolEntry *, SymbolEntry *>, uint64_t, SymbolEntryPairComp>
      CountersBySymbol;

  // Functions that have profiles.
  set<StringRef> FuncsWithProf;

  // Whether it is Position Independent Executable. If so, addresses from perf
  // file must be adjusted to map to symbols.
  bool BinaryIsPIE;
  // MMap entries, pid -> BinaryLoadMap
  map<uint64_t, set<MMapEntry>> BinaryMMapByPid;
  // All binary mmaps must have the same BinaryMMapName.
  string                  BinaryMMapName;
  // Nullptr if build id does not exist for BinaryMMapName.
  string                  BinaryBuildId;
  int32_t                 PerfDataFileParsed;
  uint64_t                SymbolsWritten;
  uint64_t                BranchesWritten;
  uint64_t                FallthroughsWritten;
  bool findBinaryBuildId();
  bool setupMMaps(quipper::PerfParser &Parser, const string &PName);
  bool setupBinaryMMapName(quipper::PerfReader &R, const string &PName);
  bool checkBinaryMMapConflictionAndEmplace(uint64_t LoadAddr,
                                            uint64_t LoadSize,
                                            uint64_t PageOffset,
                                            set<MMapEntry> &M);

  uint64_t adjustAddressForPIE(uint64_t Pid, uint64_t Addr) const {
    auto I = BinaryMMapByPid.find(Pid);
    if (I == BinaryMMapByPid.end()) {
      return INVALID_ADDRESS;
    }
    const MMapEntry *MMap{nullptr};
    for (const MMapEntry &P : I->second)
      if (P.LoadAddr <= Addr && Addr < P.LoadAddr + P.LoadSize)
        MMap = &P;
    if (!MMap) {
      // fprintf(stderr, "!!! %ld 0x%lx 0x%lx\n", Pid, Addr);
      return INVALID_ADDRESS;
    }
    if (BinaryIsPIE) {
      return Addr - MMap->LoadAddr + MMap->PageOffset;
    }
    return Addr;
  }

  void aggregateLBR(quipper::PerfParser &Parser);

  bool initBinaryFile();
  bool populateSymbolMap();
  bool parsePerfData();
  bool parsePerfData(const string &PName);
  void writeFuncList(ofstream &fout);
  void writeSymbols(ofstream &fout);
  void writeBranches(ofstream &fout);
  void writeFallthroughs(ofstream &fout);

  SymbolEntry * findSymbolAtAddress(uint64_t Pid, uint64_t Addr);

  static const uint64_t INVALID_ADDRESS = uint64_t(-1);
};

#endif
#endif
