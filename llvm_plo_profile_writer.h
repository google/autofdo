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
// 15 4e8 0 N_init
// 13 520 2b N_start
// 1 550 0 Nderegister_tm_clones
// 2 580 0 Nregister_tm_clones
// 3 5c0 0 N__do_global_dtors_aux
// 4 600 0 Nframe_dummy
// 11 610 2c Ncompute_flag
// 14 640 7c Nmain
// 5 670 f 14.1
// 6 67f 28 14.2
// 7 6a7 b 14.3
// 8 6b2 a 14.4
// 12 6c0 65 N__libc_csu_init
// 9 730 2 N__libc_csu_fini
// 10 734 0 N_fini
// 17391 19299de0 5e N_ZN9assistantD2Ev/_ZN9assistantD1Ev
// 17392 19299dff c 17391.1
// 17393 19299e0b d 17391.2
// 17394 19299e18 d 17391.3
// 17395 19299e25 19 17391.4
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
//   index    - in decimal, unique for each symbol
//   address  - in hex, without "0x"
//   size     - in hex, without "0x"
//   name     - either starts with "N" or a digit. In the former case,
//              everything after N is the symbol name. In the latter case, it's
//              in the form of "a.b", "a" is a symbol index, "b" is the bb
//              identification string (could be an index number). For the above
//              example, name "14.2" means "main.bb.2", because "14" points to
//              symbol main. Also note, symbols could have aliases, in such
//              case, aliases are concatenated with the original name with a
//              '/'. For example, symbol 17391 contains 2 aliases.
//
// Each line in "Branches" section contains the following field:
//   from     - sym_index, in decimal
//   to       - sym_index, in decimal
//   cnt      - counter, in decimal
//   C        - a field indicate this is a function call, instead of a jmp.
//
// Each line in "Fallthroughs" section contains exactly the same fields as in
// "Branches" section, except the "C" field.

class PLOProfileWriter {
public:
  struct SymbolEntry {
    SymbolEntry(uint64_t O, const StringRef &N, uint64_t A, uint64_t S,
                uint8_t T)
        : Ordinal(O), Name(N), Aliases(), Addr(A), Size(S), Type(T),
          isBBSymbol(false), ContainingFunc(nullptr) {}
    ~SymbolEntry() {}

    uint64_t Ordinal;
    StringRef Name;
    SmallVector<StringRef, 3> Aliases;
    uint64_t Addr;
    uint64_t Size;
    uint8_t  Type;

    // For basicblock symbols (e.g. "foo.bb.5"), this is the function name
    // "foo". For non basicblock symbols, this is "".
    bool isBBSymbol;
    SymbolEntry *ContainingFunc;

    // Given a basicblock symbol (e.g. "foo.bb.5"), return bb index "5".
    // Need to be changed if we use another bb label schema.
    StringRef getBBIndex() const {
      assert(isBBSymbol);
      return Name.rsplit('.').second;
    }

    bool containsAddress(uint64_t A) const {
      return Addr <= A && A < Addr + Size;
    }

    bool containsAnotherSymbol(SymbolEntry *O) const {
      if (O->Size == 0) {
        // Note if O's size is 0, we allow O on the end boundary. For example,
        // if foo.bb.4 is at address 0x10. foo is [0x0, 0x10), we then assume
        // foo contains foo.bb.4.
        return this->Addr <= O->Addr && O->Addr <= this->Addr + this->Size;
      }
      return containsAddress(O->Addr) &&
             containsAddress(O->Addr + O->Size - 1);
    }

    bool operator < (const SymbolEntry &Other) const {
      return this->Ordinal < Other.Ordinal;
    }

    bool isFunction() const {
      return this->Type == llvm::object::SymbolRef::ST_Function;
    }

    // This might be changed if we use a different bb name scheme.
    bool isFunctionForBBName(StringRef BBName) {
      if (BBName.startswith(Name)) return true;
      for (auto N : Aliases) {
        if (BBName.startswith(N)) return true;
      }
      return false;
    }
  };

  PLOProfileWriter(const string &BFN, const string &PFN, const string &OFN);
  ~PLOProfileWriter();

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
