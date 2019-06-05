#include "config.h"
#if defined(HAVE_LLVM)
#ifndef _LLVM_PLO_PROFILE_WRITER_H_
#define _LLVM_PLO_PROFILE_WRITER_H_

#include <fstream>
#include <list>
#include <map>
#include <memory>
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
// 20 c 19.1
// 21 d 19.2
// 22 d 19.3
// 23 19 19.4
// Branches
// 11 5 232381
// 5 11 234632 C
// 5 7 143535
// 7 5 226796
// Fallthroughs
// 5 5 362482
// 5 7 77142
// 5 11 2201
// 7 5 1446
// 7 7 140643
// 11 5 2221
// 11 11 225042
//
// The file consists of 3 parts, "Symbols" and "Branches".
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
//   C        - a field indicate this is a function call, instead of a jmp.
//
// Each line in "Fallthroughs" section contains exactly the same fields as in
// "Branches" section, except the "C" field.

class PropellerProfWriter {
public:
  
  PropellerProfWriter(const string &BFN, const string &PFN, const string &OFN);
  ~PropellerProfWriter();

  bool write();

 private:
  const string BinaryFileName;
  const string PerfFileName;
  const string OutFileName;

  // BinaryFileContent must be the last one to be destroyed.
  // So it appears first in this section.
  unique_ptr<llvm::MemoryBuffer>       BinaryFileContent;
  unique_ptr<llvm::object::ObjectFile> ObjectFile;
  // All symbol handlers.
  list<unique_ptr<SymbolEntry>>      SymbolList;
  // Symbol start address -> Symbol list.
  map<uint64_t, list<SymbolEntry *>> AddrMap;
  // Aggregated branch counters. <from, to> -> count.
  map<pair<uint64_t, uint64_t>, uint64_t> BranchCounters;
  map<pair<uint64_t, uint64_t>, uint64_t> FallthroughCounters;
  
  // Whether it is Position Independent Executable. If so, addresses from perf
  // file must be adjusted to map to symbols.
  bool BinaryIsPIE;
  // MMap entries, load Addr -> Size.
  map<uint64_t, uint64_t> BinaryMMaps;
  // All binary mmaps must have the same BinaryMMapName.
  string                  BinaryMMapName;
  // Nullptr if build id does not exist for BinaryMMapName.
  string                  BinaryBuildId;
  bool setupMMaps(quipper::PerfParser &Parser);
  bool setupBinaryBuildId(quipper::PerfReader &R);

  uint64_t findLoadAddress(uint64_t Addr) const {
    for (auto &P : BinaryMMaps)
      if (P.first <= Addr && Addr < P.first + P.second) return P.first;
    return INVALID_ADDRESS;
  }

  uint64_t adjustAddressForPIE(uint64_t Addr) const {
      uint64_t LoadAddress = findLoadAddress(Addr);
      if (LoadAddress == INVALID_ADDRESS) return INVALID_ADDRESS;
      if (BinaryIsPIE) return Addr - LoadAddress;
      return Addr;
  }

  void aggregateLBR(quipper::PerfParser &Parser);

  bool initBinaryFile();
  bool populateSymbolMap();
  bool parsePerfData();
  bool compareBuildId();
  void writeSymbols(ofstream &fout);
  void writeBranches(ofstream &fout);
  void writeFallthroughs(ofstream &fout);

  SymbolEntry * findSymbolAtAddress(const uint64_t Addr);

  static bool isBBSymbol(const StringRef &Name);

  static const uint64_t INVALID_ADDRESS = uint64_t(-1);
};

#endif
#endif
