#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_plo_profile_writer.h"

#include <fstream>
#include <functional>
#include <iomanip>
#include <ios>
#include <list>
#include <sstream>
#include <string>
#include <utility>

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

using llvm::StringRef;
using std::string;

PLOProfileWriter::PLOProfileWriter(const string &BFN,
                                   const string &PFN,
                                   const string &OFN)
    : BinaryFileName(BFN), PerfFileName(PFN), OutFileName(OFN) {}

PLOProfileWriter::~PLOProfileWriter() {}

bool PLOProfileWriter::isBBSymbol(const StringRef &Name, StringRef &FuncName) {
  FuncName = "";
  auto S1 = Name.rsplit('.');
  if (S1.second.empty()) return false;
  for (const char C : S1.second)
    if (C < '0' || C > '9') return false;
  auto S2 = S1.first.rsplit('.');
  if (S2.second == "bb") {
    FuncName = S2.first;
    return true;
  }
  return false;
}

PLOProfileWriter::Addr2SymCacheHandler PLOProfileWriter::findSymbolEntry(
    uint64_t OriginAddr) {
  auto CacheResult = Addr2SymCache.find(OriginAddr);
  if (CacheResult != Addr2SymCache.end()) return CacheResult;

  // If the given address is within binary mmap.
  uint64_t LoadAddr = findLoadAddress(OriginAddr);
  if (LoadAddr == INVALID_ADDRESS) return Addr2SymCache.end();

  uint64_t Addr = adjustAddressForPIE(OriginAddr);

  auto U = AddrMap.upper_bound(Addr);
  if (U == AddrMap.begin()) return Addr2SymCache.end();
  auto R = std::prev(U);

  std::list<SymbolEntry *> Candidates;
  for (auto &SymEnt : R->second)
    if (SymEnt->conainsAddress(Addr))
      Candidates.emplace_back(SymEnt.get());

  if (Candidates.empty()) return Addr2SymCache.end();

  // Sort candidates by symbol size.
  Candidates.sort([](const SymbolEntry *S1, const SymbolEntry *S2) {
    if (S1->Size != S2->Size)
      return S1->Size < S2->Size;
    return S1->Name < S2->Name;
  });

  // Some sort of disambiguation is needed for symbols that start from same
  // address and contain addr.

  // 1. If one of them is Func, the other is like Func.bb.1, return Func.bb.1.
  if (Candidates.size() == 2) {
    auto *Sym = *(Candidates.begin());
    StringRef FuncName;
    if (isBBSymbol(Sym->Name, FuncName) &&
        FuncName == (*(Candidates.rbegin()))->Name) {
      // Since, Func.Size > Func.bb.1.size, "Func.bb.1" must be the first of
      // "Candidates".
      Candidates.pop_back();
    }
  }

  // 2. Whether Candidates are a group of ctors or dtors that after demangling,
  // have exactly the same name. Itanium c++ abi - 5.1.4.3 Constructors and
  // Destructors, there are 3 variants of ctors and dtors. <ctor-dtor-name>
  //  ::= C1	# complete object constructor
  //  ::= C2	# base object constructor
  //  ::= C3	# complete object allocating constructor
  //  ::= D0	# deleting destructor
  //  ::= D1	# complete object destructor
  //  ::= D2	# base object destructor
  if (Candidates.size() <= 3 &&
      (*Candidates.front()).Size == (*Candidates.back()).Size) {
    // Helper lambdas.
    auto findUniqueDiffSpot = [](SymbolEntry *S1, SymbolEntry *S2) -> int {
      if (S1->Name.size() != S2->Name.size())
        return -1;
      int DiffSpot = -1;
      for (auto P = S1->Name.begin(), Q = S2->Name.begin(), E = S1->Name.end();
           P != E; ++P, ++Q) {
        if (*P != *Q) {
          if (DiffSpot != -1)
            return -1;
          DiffSpot = P - S1->Name.begin();
        }
      }
      assert(DiffSpot != -1);
      return DiffSpot;
    };

    auto isSameCtorOrDtor = [](StringRef &N1, StringRef &N2, int Spot) -> bool {
      auto N1N = N1[Spot], N2N = N2[Spot];
      auto N1CD = N1[Spot - 1], N2CD = N2[Spot - 1];
      return ('0' <= N1N && N1N <= '3' && '0' <= N2N && N2N <= '3' &&
              (N1CD == 'C' || N1CD == 'D') && N1CD == N2CD);
    };

    bool inSameGroup = true;
    for (auto I = Candidates.begin(), E = Candidates.end();
         inSameGroup && I != E; ++I) {
      for (auto J = std::next(I); J != E; ++J) {
        int DiffSpot = findUniqueDiffSpot(*I, *J);
        if (DiffSpot <= 0 ||
            !isSameCtorOrDtor((*I)->Name, (*J)->Name, DiffSpot)) {
          inSameGroup = false;
          break;
        }
      }
    }
    if (inSameGroup) {
      auto *Chosen = Candidates.front();
      Candidates.clear();
      Candidates.push_back(Chosen);
    }
  } // End of disambiguating Ctor / Dtor groups.

  if (Candidates.size() > 1) {
    // If we hit this very rare case, log it up.
    std::stringstream ss;
    ss << "Multiple symbols may contain adderss: " << std::endl;
    ss << std::hex << std::showbase;
    for (auto &SE : Candidates) {
      ss << "\t"
         << "Addr=" << SE->Addr << ", Size=" << SE->Size
         << ", Name=" << SE->Name.str() << std::endl;
    }
    // Issue the warning in one shot, not multiple warnings.
    LOG(INFO) << ss.str();
  }

  auto InsertR = Addr2SymCache.emplace(OriginAddr, std::move(Candidates));
  assert(InsertR.second);
  return InsertR.first;
}

namespace {
struct DecOut {
} dec;

struct HexOut {
} hex;

static std::ostream &operator<<(std::ostream &out, const struct DecOut &) {
  return out << std::dec << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const struct HexOut &) {
  return out << std::hex << std::noshowbase;
}

static std::ostream &
operator<<(std::ostream &out,
           const std::list<PLOProfileWriter::SymbolEntry *> &Syms) {
  assert(!Syms.empty());
  for (auto *P : Syms) {
    if (P != Syms.front())
      out << "/";
    out << dec << P->Ordinal;
  }
  return out;
}

struct BuildIdWrapper {
  BuildIdWrapper(const quipper::PerfDataProto_PerfBuildID &BuildId)
      : Data(BuildId.build_id_hash().c_str()) {}
  
  BuildIdWrapper(const char *P) : Data(P) {}
    
  const char *Data;
};

static std::ostream &
operator<<(std::ostream &out, const BuildIdWrapper &BW) {
    const char *p = BW.Data;
    for (int i = 0; i < quipper::kBuildIDArraySize; ++i, ++p) {
      out << std::setw(2) << std::setfill('0') << std::hex
          << ((int)(*p) & 0xFF);
    }
  return out;
}
} // namespace

bool PLOProfileWriter::write() {
  if (!parsePerfData() || !initBinaryFile() || !populateSymbolMap()) {
    return false;
  }
  std::ofstream fout(OutFileName);
  if (fout.bad()) {
    LOG(ERROR) << "Failed to open '" << OutFileName << "' for writing.";
    return false;
  }
  writeSymbols(fout);
  writeBranches(fout);
  return true;
}

void PLOProfileWriter::writeSymbols(std::ofstream &fout) {
  fout << "Symbols" << std::endl;
  for (auto &LE : AddrMap) {
    for (auto &SEPtr : LE.second) {
      SymbolEntry &SE = *SEPtr;
      fout << dec << SE.Ordinal << " "
           << hex << SE.Addr << " " << SE.Size << " ";
      if (SE.BBFuncName.empty()) {
        fout << "N" << SE.Name.str() << std::endl;
      } else {
        auto R = NameMap.find(SE.BBFuncName);
        assert(R != NameMap.end());
        SymbolEntry *BBFuncSym = R->second;
        fout << dec << BBFuncSym->Ordinal << "." << SE.getBBIndex().str()
             << std::endl;
      }
    }
  }
}

void PLOProfileWriter::writeBranches(std::ofstream &fout) {
  fout << "Branches" << std::endl;
  for (auto &EC : EdgeCounters) {
    const uint64_t From = EC.first.first;
    const uint64_t To = EC.first.second;
    const uint64_t Cnt = EC.second;
    auto FromSymHandler = findSymbolEntry(From);
    auto ToSymHandler = findSymbolEntry(To);
    if (FromSymHandler != Addr2SymCache.end() &&
        ToSymHandler != Addr2SymCache.end()) {
      fout << FromSymHandler->second << " " 
           << ToSymHandler->second << " "
           << dec << Cnt << " " 
           << hex << adjustAddressForPIE(From) << " "
           << hex << adjustAddressForPIE(To) << std::endl;
    }
  }
}

bool PLOProfileWriter::initBinaryFile() {
  auto FileOrError = llvm::MemoryBuffer::getFile(BinaryFileName);
  if (!FileOrError) {
      LOG(ERROR) << "Failed to read file '" << BinaryFileName << "'.";
      return false;
  }
  this->BinaryFileContent = std::move(*FileOrError);

  auto ObjOrError = llvm::object::ObjectFile::createELFObjectFile(
      llvm::MemoryBufferRef(*(this->BinaryFileContent)));
  if (!ObjOrError) {
    LOG(ERROR) << "Not a valid ELF file '" << BinaryFileName << "'.";
    return false;
  }
  this->ObjectFile = std::move(*ObjOrError);

  llvm::object::ELFObjectFileBase *ELFObj =
      llvm::dyn_cast<llvm::object::ELFObjectFileBase, llvm::object::ObjectFile>(
          ObjectFile.get());
  BinaryIsPIE = (ELFObj->getEType() == llvm::ELF::ET_DYN);
  LOG(INFO) << "'" << this->BinaryFileName
               << "' is PIE binary: " << BinaryIsPIE;

  if (!this->BinaryBuildId) return true;

  // Extract build id and compare it against perf build id.
  bool BuildIdFound = false;
  bool BuildIdMatch = true;
  for (auto &SR : ObjectFile->sections()) {
    llvm::object::ELFSectionRef ESR(SR);
    StringRef SName;
    StringRef SContents;
    if (ESR.getType() == llvm::ELF::SHT_NOTE &&
        SR.getName(SName).value() == 0 && SName == ".note.gnu.build-id" &&
        SR.getContents(SContents).value() == 0 &&
        SContents.size() > quipper::kBuildIDArraySize) {
      BuildIdFound = true;
      auto P = SContents.end();
      auto Q = BinaryBuildId.get() + quipper::kBuildIDArraySize;
      for (int i = 1; i <= quipper::kBuildIDArraySize; ++i) {
        if (*(--P) != *(--Q)) {
          BuildIdMatch = false;
          break;
        }
      }
      if (BuildIdMatch) {
        LOG(INFO) << "Found Build Id in binary '" << BinaryFileName
                  << "': " << BuildIdWrapper(P);
      }
    }
  }
  if (!BuildIdFound) {
    LOG(INFO) << "No Build Id found in '" << this->BinaryFileName << "'.";
  } else {
    if (BuildIdMatch) {
      LOG(INFO) << "Found Build Id in binary, and it matches perf data.";
    } else {
      LOG(ERROR) << "Build Ids do not match.";
      return false;
    }
  }
  return true;
}

bool PLOProfileWriter::populateSymbolMap() {
  uint64_t SymbolOrdinal = 0;
  auto Symbols = ObjectFile->symbols();
  for (const auto &Sym : Symbols) {
    auto AddrR = Sym.getAddress();
    auto SecR = Sym.getSection();
    auto NameR = Sym.getName();

    if (!(AddrR && *AddrR && SecR && (*SecR)->isText() && NameR)) continue;

    StringRef Name = *NameR;
    uint64_t Addr = *AddrR;
    if (!Name.endswith(".bbend")) {
      auto &L = AddrMap[Addr];
      L.emplace_back(
          new SymbolEntry(++SymbolOrdinal, Name, Addr,
                          llvm::object::ELFSymbolRef(Sym).getSize()));
      StringRef BBFuncName;
      if (isBBSymbol(Name, BBFuncName)) {
        L.back()->BBFuncName = BBFuncName;
      } else {
        NameMap[Name] = L.back().get();
      }
    }
  }
  return true;
}

bool PLOProfileWriter::parsePerfData() {
  quipper::PerfReader PR;
  if (!PR.ReadFile(PerfFileName)) {
    LOG(ERROR) << "Failed to read perf data file: " << PerfFileName;
    return false;
  }

  quipper::PerfParser Parser(&PR);
  if (!Parser.ParseRawEvents()) {
    LOG(ERROR) << "Failed to parse perf raw events.";
    return false;
  }

  if (!setupMMaps(Parser)) {
    LOG(ERROR) << "Failed to find perf mmaps for binary '" << BinaryFileName
               << "'.";
    return false;
  }

  setupBinaryBuildId(PR);

  aggregateLBR(Parser);
  return true;
}

bool PLOProfileWriter::setupMMaps(quipper::PerfParser &Parser) {
  // Depends on the binary file name, if
  //   - it is absolute, compares it agains the full path
  //   - it is relative, only compares the file name part
  // Note: CompFunc is constructed in a way so that there is no branch /
  // conditional test inside the function.
  struct BinaryNameComparator {
    BinaryNameComparator(const string &BinaryFileName) {
      if (llvm::sys::path::is_absolute(BinaryFileName)) {
        ComparePart = StringRef(BinaryFileName);
        PathChanger = NullPathChanger;
      } else {
        ComparePart = llvm::sys::path::filename(BinaryFileName);
        PathChanger = NameOnlyPathChanger;
      }
    }

    bool operator()(const string &Path) {
      return ComparePart == PathChanger(Path);
    }

    StringRef ComparePart;
    std::function<StringRef(const string &)> PathChanger;

    static StringRef NullPathChanger(const string &S) { return StringRef(S); }
    static StringRef NameOnlyPathChanger(const string &S) {
      return llvm::sys::path::filename(StringRef(S));
    }
  } CompFunc(this->BinaryFileName);

  this->BinaryMMapName = "";
  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() != quipper::PerfDataProto_PerfEvent::kMmapEvent)
      continue;

    const quipper::PerfDataProto_MMapEvent &MMap = EPtr->mmap_event();
    if (!MMap.has_filename()) continue;

    const string &MMapFileName = MMap.filename();
    if (!CompFunc(MMapFileName) || !MMap.has_start() || !MMap.has_len())
      continue;

    if (this->BinaryMMapName.empty()) {
      this->BinaryMMapName = MMapFileName;
    } else if (BinaryMMapName != MMapFileName) {
      LOG(ERROR) << "'" << BinaryFileName
                 << "' is not specific enough. It matches both '"
                 << BinaryMMapName << "' and '" << MMapFileName
                 << "' in the perf data file. Consider using absolute "
                    "file name.";
      return false;
    }
    uint64_t LoadAddr = MMap.start();
    uint64_t LoadSize = MMap.len();
    // Check for mmap conflicts.
    bool CheckOk = true;
    if (!BinaryMMaps.empty()) {
      auto UpperMMap = BinaryMMaps.find(LoadAddr);
      if (UpperMMap == BinaryMMaps.end()) {
        auto E = std::prev(UpperMMap);
        CheckOk = (E->first + E->second <= LoadAddr);
      } else if (UpperMMap != BinaryMMaps.begin()) {
        auto LowerMMap = std::prev(UpperMMap);
        // Now LowerMMap->first <= LoadAddr && LoadAddr < UpperMMap->first
        CheckOk =
            (LowerMMap->first == LoadAddr && LowerMMap->second == LoadSize) ||
            (LowerMMap->first + LowerMMap->second <= LoadAddr &&
             LoadAddr + LoadSize < UpperMMap->first);
      }
    }

    if (CheckOk) {
      BinaryMMaps[LoadAddr] = LoadSize;
    } else {
      LOG(ERROR) << "Found conflict MMap event: [" << hex << LoadAddr << ", "
                 << hex << LoadAddr + LoadSize << ").";
      return false;
    }
  }  // End of iterating mmap events.

  if (BinaryMMaps.empty()) {
    LOG(ERROR) << "Failed to find mmap entry for '" << BinaryFileName << "'. '"
               << PerfFileName << "' does not match '" << BinaryFileName
               << "'.";
    return false;
  }
  for (auto &M : BinaryMMaps) {
    LOG(INFO) << "Find mmap for binary: '" << BinaryFileName
              << "', start mapping address=" << hex << M.first
              << ", mapped size=" << M.second << ".";
  }
  return true;
}

bool PLOProfileWriter::setupBinaryBuildId(quipper::PerfReader &PR) {
  for (const auto &BuildId: PR.build_ids()) {
    if (BuildId.has_filename() && BuildId.has_build_id_hash() &&
        BuildId.filename() == BinaryMMapName) {
      BinaryBuildId.reset(new char[quipper::kBuildIDArraySize]);
      memcpy(BinaryBuildId.get(), BuildId.build_id_hash().c_str(),
             quipper::kBuildIDArraySize);
      LOG(INFO) << "Found Build Id in perf data '" << PerfFileName
                << "': " << BuildId;
      return true;
    }
  }
  LOG(INFO) << "No Build Id info for '" << this->BinaryFileName
            << "' found in '" << this->PerfFileName << "'.";
  BinaryBuildId.reset(nullptr);
  return false;
}

void PLOProfileWriter::aggregateLBR(quipper::PerfParser &Parser) {
  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() ==
        quipper::PerfDataProto_PerfEvent::kSampleEvent) {
      for (const auto &BE : EPtr->sample_event().branch_stack()) {
        ++(EdgeCounters[std::make_pair(BE.from_ip(), BE.to_ip())]);
      }
    }
  }
}

#endif
