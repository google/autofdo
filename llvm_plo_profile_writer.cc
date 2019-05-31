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
using std::list;
using std::ofstream;
using std::string;

PLOProfileWriter::PLOProfileWriter(const string &BFN,
                                   const string &PFN,
                                   const string &OFN)
    : BinaryFileName(BFN), PerfFileName(PFN), OutFileName(OFN) {}

PLOProfileWriter::~PLOProfileWriter() {}

bool PLOProfileWriter::isBBSymbol(const StringRef &Name) {
  auto S1 = Name.rsplit('.');
  if (S1.second.empty()) return false;
  for (const char C : S1.second)
    if (C < '0' || C > '9') return false;
  auto S2 = S1.first.rsplit('.');
  if (S2.second == "bb") {
    return true;
  }
  return false;
}

list<PLOProfileWriter::SymbolEntry *> *
PLOProfileWriter::findSymbolAtAddress(uint64_t OriginAddr) {
  uint64_t Addr = adjustAddressForPIE(OriginAddr);
  if (Addr == INVALID_ADDRESS) return nullptr;
  auto ResolvedResult = ResolvedAddrToSymMap.find(Addr);
  if (ResolvedResult != ResolvedAddrToSymMap.end())
    return &(ResolvedResult->second);

  auto U = AddrMap.upper_bound(Addr);
  if (U == AddrMap.begin()) return nullptr;
  auto R = std::prev(U);

  // 95+% of the cases:
  if (R->second.size() == 1 && R->second.front()->containsAddress(Addr))
    return &(R->second);

  list<SymbolEntry *> Candidates;
  for (auto &SymEnt : R->second)
    if (SymEnt->containsAddress(Addr))
      Candidates.emplace_back(SymEnt);

  if (Candidates.empty()) return nullptr;

  // Some sort of disambiguation is needed for symbols that start from same
  // address and contain addr.

  // 0. Sort candidates by symbol size.
  Candidates.sort([](const SymbolEntry *S1, const SymbolEntry *S2) {
    if (S1->Size != S2->Size)
      return S1->Size < S2->Size;
    return S1->Name < S2->Name;
  });

  // 1. If one of them is Func, the other is like Func.bb.1, return Func.bb.1.
  if (Candidates.size() == 2) {
    auto *Sym = *(Candidates.begin());
    StringRef FuncName;
    if (Sym->isBBSymbol &&
        Sym->ContainingFuncSymbol == (*(Candidates.rbegin()))) {
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
    ss << std::hex << std::showbase;
    ss << "Multiple symbols may contain adderss: " << Addr << std::endl;
    for (auto &SE : Candidates) {
      ss << "\t"
         << "Addr=" << SE->Addr << ", Size=" << SE->Size
         << ", Name=" << SE->Name.str() << std::endl;
    }
    // Issue the warning in one shot, not multiple warnings.
    LOG(INFO) << ss.str();
  }

  auto InsertR = ResolvedAddrToSymMap.emplace(
      std::piecewise_construct, std::forward_as_tuple(Addr),
      std::forward_as_tuple(std::move(Candidates)));
  assert(InsertR.second);
  return &(InsertR.first->second);
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
    for (int i = 0; i < quipper::kBuildIDArraySize; ++i) {
      out << std::setw(2) << std::setfill('0') << std::hex
          << ((int)(BW.Data[i]) & 0xFF);
    }
  return out;
}
} // namespace

bool PLOProfileWriter::write() {
  if (!initBinaryFile() || !populateSymbolMap() || !parsePerfData() ||
      !compareBuildId()) {
    return false;
  }
  ofstream fout(OutFileName);
  if (fout.bad()) {
    LOG(ERROR) << "Failed to open '" << OutFileName << "' for writing.";
    return false;
  }
  writeSymbols(fout);
  writeBranches(fout);
  // writeFallthroughs(fout);
  return true;
}

void PLOProfileWriter::writeSymbols(ofstream &fout) {
  fout << "Symbols" << std::endl;
  for (auto &LE : AddrMap) {
    for (auto &SEPtr : LE.second) {
      SymbolEntry &SE = *SEPtr;
      fout << dec << SE.Ordinal << " "
           << hex << SE.Addr << " " << SE.Size << " ";
      if (!SE.isBBSymbol) {
        fout << "N" << SE.Name.str() << std::endl;
      } else {
        fout << dec << SE.ContainingFuncSymbol->Ordinal << "."
             << SE.getBBIndex().str() << std::endl;
      }
    }
  }
}

void PLOProfileWriter::writeBranches(std::ofstream &fout) {
  fout << "Branches" << std::endl;
  for (auto &EC : BranchCounters) {
    const uint64_t From = EC.first.first;
    const uint64_t To = EC.first.second;
    const uint64_t Cnt = EC.second;
    auto FromSymHandler = findSymbolAtAddress(From);
    auto ToSymHandler = findSymbolAtAddress(To);
    if (FromSymHandler && ToSymHandler) {
      fout << *FromSymHandler << " " << *ToSymHandler << " " << dec << Cnt;
      auto *ToSym = ToSymHandler->front();
      const uint64_t AdjustedTo = adjustAddressForPIE(To);
      if ((ToSym->isBBSymbol && ToSym->ContainingFuncSymbol->Addr == AdjustedTo) ||
      (!ToSym->isBBSymbol && ToSym->Addr == AdjustedTo)) {
          fout << " C";
      }
      fout << std::endl;
    }
  }
}

void PLOProfileWriter::writeFallthroughs(std::ofstream &fout) {
  fout << "Fallthroughs" << std::endl;
  // for (auto &EC : FallthroughCounters) {
  //   const auto *From = EC.first.first;
  //   const auto *To = EC.first.second;
  //   const uint64_t Cnt = EC.second;
  //   fout << dec << *From << " " << *To << " " << Cnt << " " << std::endl;
  // }
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
  return true;
}

bool PLOProfileWriter::populateSymbolMap() {
  uint64_t SymbolOrdinal = 0;
  auto Symbols = ObjectFile->symbols();
  for (const auto &Sym : Symbols) {
    auto AddrR = Sym.getAddress();
    auto SecR = Sym.getSection();
    auto NameR = Sym.getName();
    auto TypeR = Sym.getType();

    if (!(AddrR && *AddrR && SecR && (*SecR)->isText() && NameR && TypeR))
      continue;

    StringRef Name = *NameR;
    uint64_t Addr = *AddrR;
    uint8_t Type(*TypeR);

    llvm::object::ELFSymbolRef ELFSym(Sym);
    SymbolEntry *NewSymbolEntry =
        new SymbolEntry(++SymbolOrdinal, Name, Addr, ELFSym.getSize(), Type);
    SymbolList.emplace_back(NewSymbolEntry);
    auto &L = AddrMap[Addr];
    L.push_back(NewSymbolEntry);
    if (!isBBSymbol(Name)) {
      NameMap[Name] = L.back();
      NewSymbolEntry->isBBSymbol = false;
    } else {
      NewSymbolEntry->isBBSymbol = true;
    }
  }

  // Now scan all the symbols in address order.
  decltype (AddrMap)::iterator LastFuncPos = AddrMap.end();
  for (auto P = AddrMap.begin(), Q = AddrMap.end(); P != Q; ++P) {
    uint64_t Addr = P->first;

    for (auto *S : P->second) {
      if (S->isFunction() && !S->isBBSymbol) {
        LastFuncPos = P;
        break;
      }
    }

    if (LastFuncPos == AddrMap.end())  continue;
    for (auto *S : P->second) {
      if (!S->isBBSymbol)
        continue;
      // this is a bb symbol, find a wrapping func for it.
      SymbolEntry *ContainingFunc = nullptr;
      for (auto *FP : LastFuncPos->second) {
        if (FP->isFunction() && !FP->isBBSymbol &&
            FP->containsAnotherSymbol(S) && S->Name.startswith(FP->Name)) {
          if (ContainingFunc == nullptr) {
            ContainingFunc = FP;
          } else if (ContainingFunc->Size < S->Size) {
            // Already has a containing function, choose the one that is
            // larger.
            ContainingFunc = S;
          }
        }
      }
      if (!ContainingFunc) {
        // Disambiguate the following case:
        // 0x10 foo       size = 2
        // 0x12 foo.bb.1  size = 2
        // 0x14 foo.bb.2  size = 0
        // 0x14 bar  <- LastFuncPos set is to bar.
        // In this scenario, we seek lower address.
        auto T = LastFuncPos;
        while (T != AddrMap.begin()) {
          T = std::prev(T);
          for (auto *KS : T->second) {
            if (KS->isFunction() && !KS->isBBSymbol &&
                KS->containsAnotherSymbol(S) && S->Name.startswith(KS->Name)) {
              ContainingFunc = KS;
              break;
            }
          }
        }
      }
      S->ContainingFuncSymbol = ContainingFunc;
      if (S->ContainingFuncSymbol == nullptr) {
        LOG(ERROR) << "Failed to find function for bb symbol: " << S->Name.str()
                   << " @ 0x" << hex << S->Addr;
        return false;
      } else {
        if (!S->Name.startswith(ContainingFunc->Name)) {
          LOG(ERROR) << "Internal check warning: \n"
                     << "Sym: " << S->Name.str() << "\n"
                     << "Func: " << S->ContainingFuncSymbol->Name.str();
        }
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
      BinaryBuildId = BuildId.build_id_hash();
      quipper::PerfizeBuildIDString(&BinaryBuildId);
      LOG(INFO) << "Found Build Id in perf data '" << PerfFileName
                << "': " << BuildIdWrapper(BinaryBuildId.c_str());
      return true;
    }
  }
  LOG(INFO) << "No Build Id info for '" << this->BinaryFileName
            << "' found in '" << this->PerfFileName << "'.";
  BinaryBuildId = "";
  return false;
}

void PLOProfileWriter::aggregateLBR(quipper::PerfParser &Parser) {
  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() ==
        quipper::PerfDataProto_PerfEvent::kSampleEvent) {
      auto BRStack = EPtr->sample_event().branch_stack();
      if (BRStack.empty()) continue;
      uint64_t LastFrom = INVALID_ADDRESS;
      uint64_t LastTo = INVALID_ADDRESS;
      list<SymbolEntry *> *LastToSyms = nullptr;
      for (int P = BRStack.size() - 1; P >= 0; --P) {
        const auto &BE = BRStack.Get(P);
        uint64_t From = BE.from_ip();
        uint64_t To = BE.to_ip();
        ++(BranchCounters[std::make_pair(From, To)]);
        if (LastTo != INVALID_ADDRESS) {
          ++(FallthroughCounters[std::make_pair(LastTo, From)]);
        }
        LastTo = To;
      }
    }
  }
}

bool PLOProfileWriter::compareBuildId() {
  if (this->BinaryBuildId.empty())
    return true;

  // Extract build id and compare it against perf build id.
  bool BuildIdFound = false;
  bool BuildIdMatch = true;
  for (auto &SR : ObjectFile->sections()) {
    llvm::object::ELFSectionRef ESR(SR);
    StringRef SName;
    StringRef SContents;
    if (ESR.getType() == llvm::ELF::SHT_NOTE &&
        SR.getName(SName).value() == 0 && SName == ".note.gnu.build-id" &&
        SR.getContents(SContents).value() == 0 && SContents.size()) {
      const unsigned char *P = SContents.bytes_end();
      while (*(--P) && P >= SContents.bytes_begin())
        ;
      if (P < SContents.bytes_begin()) {
        LOG(INFO) << "Section '.note.gnu.build-id' does not contain valid "
                      "build id information.";
        return true;
      }
      string BuildId((const char *)(P + 1),
                     SContents.size() - (P + 1 - SContents.bytes_begin()));
      quipper::PerfizeBuildIDString(&BuildId);
      LOG(INFO) << "Found Build Id in binary '" << BinaryFileName
                << "': " << BuildIdWrapper(BuildId.c_str());

      if (this->BinaryBuildId == BuildId) {
        LOG(INFO) << "Binary Build Id and perf data Build Id match.";
        return true;
      } else {
        LOG(ERROR) << "Build Ids do not match.";
        return false;
      }
    }
  }
  LOG(INFO) << "No Build Id found in '" << this->BinaryFileName << "'.";
  return true;
}

#endif
