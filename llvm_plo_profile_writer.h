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

#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

using std::list;
using std::map;
using std::string;
using std::unique_ptr;

using llvm::StringRef;

namespace quipper {
class PerfParser;
}

// This class, given binary and perf.data file paths, writes profile data
// that are to be consumed by plo optimizer.

// A sample output is like below:
//
// Symbols
// 1 4c8 0 N
// 20 4c8 0 N_init
// 2 4e0 0 N
// 3 4f0 0 N
// 4 500 0 N
// 18 500 2b N_start
// 6 530 0 Nderegister_tm_clones
// 7 560 0 Nregister_tm_clones
// 8 5a0 0 N__do_global_dtors_aux
// 9 5e0 0 Nframe_dummy
// 16 5f0 2c Ncompute_flag
// 19 620 7a Nmain
// 10 64e f 19.1
// 11 65d 28 19.2
// 12 685 b 19.3
// 13 690 a 19.4
// 17 6a0 65 N__libc_csu_init
// 14 710 2 N__libc_csu_fini
// 5 714 0 N
// 15 714 0 N_fini
// Branches
// 16 10 326983 61b 655
// 10 16 330140 650 5f0
// 10 12 206113 65b 685
// 12 10 313726 68e 64e
//
// The file consists of 2 parts, "Symbols" and "Branches".
//
// Each line in "Symbols" section contains the following field:
//   index    - in decimal, unique for each symbol
//   address  - in hex, without "0x"
//   size     - in hex, without "0x"
//   name     - either starts with "N" or a digit. In the former case,
//              everything after N is the symbol name. In the latter case, it's
//              in the form of "a.b", "a" is a symbol index, "b" is the bb
//              section number. For the above example, name "19.3" means
//              "main.bb.3", because "19" points to symbol main.
//
// Each line in "Branches" section contains the following field:
//   from      - sym_index[/sym_index_2][/sym_index_3]..., in decimal
//   to        - sym_index[/sym_index_2][/sym_index_3]..., in decimal
//   cnt       - in decimal
//   from_addr - address in hex without "0x"
//   to_addr   - address in hex without "0x"
// Sometimes, the same address corresponding to multiple symbols, in this case,
// multiple symbol index are jointed by "/". Ideally, from_addr and to_addr
// should not be present, however, they are used to differentiate whether the
// jump target is the beginning of the symbol or in the middle of it. (In the
// latter case, it might be a return from call.)

class PLOProfileWriter {
 public:
  struct SymbolEntry {
    SymbolEntry(uint64_t O, const StringRef &N, uint64_t A, uint64_t S)
        : Ordinal(O), Name(N), Addr(A), Size(S), BBFuncName("") {}
    ~SymbolEntry() {}

    uint64_t Ordinal;
    StringRef Name;
    uint64_t Addr;
    uint64_t Size;
    // For basicblock symbols (e.g. "foo.bb.5"), this is the function name
    // "foo". For non basicblock symbols, this is "".
    StringRef BBFuncName;

    // Given a basicblock symbol (e.g. "foo.bb.5"), return bb index "5".
    StringRef getBBIndex() const {
      assert(!BBFuncName.empty());
      auto t = BBFuncName.size() + 4;  // "4" is for ".bb."
      return StringRef(Name.data() + t, Name.size() - t);
    }

    bool conainsAddress(uint64_t A) const {
        return Addr <= A && A < Addr + Size;
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
  // Addr -> Symbols mapping. Note that multiple symbols having same address is
  // common.
  map<uint64_t, list<unique_ptr<SymbolEntry>>> AddrMap;
  map<StringRef, SymbolEntry *>                NameMap;
  // Aggregated branch counters. <from, to> -> count.
  map<std::pair<uint64_t, uint64_t>, uint64_t> EdgeCounters;

  // Whether it is Position Independent Executable. If so, addresses from perf
  // file must be adjusted to map to symbols.
  bool BinaryIsPIE;
  // MMap entries, load Addr -> Size.
  map<uint64_t, uint64_t> BinaryMMaps;
  bool setupMMaps(quipper::PerfParser &Parser);

  uint64_t findLoadAddress(uint64_t Addr) const {
    for (auto &P : BinaryMMaps)
      if (P.first <= Addr && Addr < P.first + P.second) return P.first;
    return INVALID_ADDRESS;
  }

  uint64_t adjustAddressForPIE(uint64_t Addr) const {
      assert(findLoadAddress(Addr) != INVALID_ADDRESS);
      if (BinaryIsPIE) return Addr - findLoadAddress(Addr);
      return Addr;
  }

  void aggregateLBR(quipper::PerfParser &Parser);

  using Addr2SymCacheTy = map<uint64_t, list<SymbolEntry *>>;
  using Addr2SymCacheHandler = Addr2SymCacheTy::iterator;
  Addr2SymCacheTy Addr2SymCache;

  bool initBinaryFile();
  bool populateSymbolMap();
  bool parsePerfData();
  void writeSymbols(std::ofstream &fout);
  void writeBranches(std::ofstream &fout);

  Addr2SymCacheHandler findSymbolEntry(const uint64_t Addr);

  // Whether Name is a bb symbol. If so, return true and set FuncName to the
  // name of function that contains the symbol.
  static bool isBBSymbol(const StringRef &Name, StringRef &FuncName);

  static const uint64_t INVALID_ADDRESS = uint64_t(-1);
};

#endif
#endif
