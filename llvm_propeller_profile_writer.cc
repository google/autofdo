#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_propeller_profile_writer.h"

#include <fstream>
#include <functional>
#include <iomanip>
#include <ios>
#include <list>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

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

using llvm::StringRef;
using std::list;
using std::ofstream;
using std::pair;
using std::string;

PropellerProfWriter::PropellerProfWriter(const string &BFN, const string &PFN,
                                         const string &OFN)
    : BinaryFileName(BFN), PerfFileName(PFN), PropOutFileName(OFN) {}

PropellerProfWriter::~PropellerProfWriter() {}

SymbolEntry *
PropellerProfWriter::findSymbolAtAddress(uint64_t OriginAddr) {
  uint64_t Addr = adjustAddressForPIE(OriginAddr);
  if (Addr == INVALID_ADDRESS) return nullptr;
  auto U = AddrMap.upper_bound(Addr);
  if (U == AddrMap.begin()) return nullptr;
  auto R = std::prev(U);

  // 99+% of the cases:
  if (R->second.size() == 1 && R->second.front()->containsAddress(Addr))
    return *(R->second.begin());

  list<SymbolEntry *> Candidates;
  for (auto &SymEnt : R->second)
    if (SymEnt->containsAddress(Addr))
      Candidates.emplace_back(SymEnt);

  if (Candidates.empty()) return nullptr;

   // Sort candidates by symbol size.
  Candidates.sort([](const SymbolEntry *S1, const SymbolEntry *S2) {
    if (S1->Size != S2->Size)
      return S1->Size < S2->Size;
    return S1->Name < S2->Name;
  });

  // Return the smallest symbol that contains address.
  return *Candidates.begin();
}

namespace {
struct DecOut {
} dec;

struct HexOut {
} hex;

struct SymBaseF {
  SymBaseF(const SymbolEntry &S) : Symbol(S) {}
  const SymbolEntry &Symbol;
};

struct SymNameF : public SymBaseF {
  SymNameF(const SymbolEntry &S) : SymBaseF(S) {}
};

struct SymOrdinalF : public SymBaseF {
  SymOrdinalF(const SymbolEntry &S) : SymBaseF(S) {}
};

struct SymSizeF : public SymBaseF {
  SymSizeF(const SymbolEntry &S) : SymBaseF(S) {}
};

struct CountF {
  CountF(uint64_t C) : Cnt(C) {};

  uint64_t Cnt;
};

static std::ostream &operator<<(std::ostream &out, const struct DecOut &) {
  return out << std::dec << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const struct HexOut &) {
  return out << std::hex << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const SymNameF &NameF) {
  auto &Sym = NameF.Symbol;
  out << Sym.Name.str();
  for (auto A : Sym.Aliases)
    out << "/" << A.str();
  return out;
}

static std::ostream &operator<<(std::ostream &out,
                                const SymOrdinalF &OrdinalF) {
  return out << dec << OrdinalF.Symbol.Ordinal;
}

static std::ostream &operator<<(std::ostream &out, const SymSizeF &SizeF) {
  return out << hex << SizeF.Symbol.Size;
}

static std::ostream &operator<<(std::ostream &out, const CountF &CountF) {
  return out << dec << CountF.Cnt;
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

bool PropellerProfWriter::write() {
  if (!initBinaryFile() || !findBinaryBuildId() || !populateSymbolMap() ||
      !parsePerfData()) {
    return false;
  }

  ofstream fout(PropOutFileName);
  if (fout.bad()) {
    LOG(ERROR) << "Failed to open '" << PropOutFileName << "' for writing.";
    return false;
  }

  writeSymbols(fout);
  writeBranches(fout);
  writeFallthroughs(fout);
  writeFuncList(fout);
  LOG(INFO) << "Wrote propeller profile (" << this->PerfDataFileParsed
            << " file(s), " << this->SymbolsWritten << " syms, "
            << this->BranchesWritten << " branches, "
            << this->FallthroughsWritten << " fallthroughs) to "
            << PropOutFileName;
  return true;
}

void PropellerProfWriter::writeFuncList(ofstream &fout) {
  for (auto &F : FuncsWithProf) {
    fout << "!" << F.str() << std::endl;
  }
  fout << "!" << std::endl;
}

void PropellerProfWriter::writeSymbols(ofstream &fout) {
  this->SymbolsWritten = 0;
  uint64_t SymbolOrdinal = 0;
  fout << "Symbols" << std::endl;
  for (auto &LE : AddrMap) {
    // Tricky case here:
    // In the same address we have:
    //    foo.bb.1
    //    foo
    // So we first output foo.bb.1, and at this time
    //   foo.bb.1->containingFunc->Index == 0.
    // We output 0.1, wrong!.
    // To handle this, we sort LE by BBTag:
    if (LE.second.size() > 1) {
      LE.second.sort([](SymbolEntry *S1, SymbolEntry *S2) {
        if (S1->BBTag != S2->BBTag) {
          return !S1->BBTag;
        }
        // order irrelevant, but choose a stable relation.
        return S1->Name < S2->Name;
      });
    }
    // Then apply ordial to all before accessing.
    for (auto *SEPtr : LE.second) {
      SEPtr->Ordinal = ++SymbolOrdinal;
    }
    for (auto *SEPtr : LE.second) {
      SymbolEntry &SE = *SEPtr;
      fout << SymOrdinalF(SE) << " " << SymSizeF(SE) << " ";
      ++this->SymbolsWritten;
      if (SE.BBTag) {
        fout << SymOrdinalF(*(SE.ContainingFunc)) << ".";
        StringRef BBIndex = SE.Name;
        fout << dec << (uint64_t)(BBIndex.bytes_end() - BBIndex.bytes_begin())
             << std::endl;
      } else {
        fout << "N" << SymNameF(SE) << std::endl;
      }
    }
  }
}

void PropellerProfWriter::writeBranches(std::ofstream &fout) {
  this->BranchesWritten = 0;
  fout << "Branches" << std::endl;
  auto recordFuncsWithProf = [this](SymbolEntry *S) {
    if (!S || !(S->ContainingFunc) || S->ContainingFunc->Name.empty())
      return;
    // Dups are properly handled by set.
    this->FuncsWithProf.insert(S->ContainingFunc->Name);
  };
  for (auto &EC : BranchCounters) {
    const uint64_t From = EC.first.first;
    const uint64_t To = EC.first.second;
    const uint64_t Cnt = EC.second;
    auto *FromSym = findSymbolAtAddress(From);
    auto *ToSym = findSymbolAtAddress(To);
    const uint64_t AdjustedTo = adjustAddressForPIE(To);
    recordFuncsWithProf(FromSym);
    recordFuncsWithProf(ToSym);
    if (FromSym && ToSym) {
      /* If a return jumps to an address associated with a BB symbol ToSym,
       * then find the actual callsite symbol which is the symbol right
       * before ToSym. */
      if (ToSym->BBTag &&
          (FromSym->ContainingFunc->Addr != ToSym->ContainingFunc->Addr) &&
          ToSym->ContainingFunc->Addr != AdjustedTo &&
          AdjustedTo == ToSym->Addr) { /* implies an inter-procedural return to
                                          the end of a basic block */
        auto *CallSiteSym = findSymbolAtAddress(To - 1);
        // LOG(INFO) << std::hex << "Return From: 0x" << From << " To: 0x" << To
        //           << " Callsite symbol: 0x"
        //           << (CallSiteSym ? CallSiteSym->Addr : 0x0) << "\n"
        //           << std::dec;
        if (CallSiteSym && CallSiteSym->BBTag) {
          /* Account for the fall-through between CallSiteSym and ToSym. */
          CountersBySymbol[std::make_pair(CallSiteSym, ToSym)] += Cnt;
          /* Reassign ToSym to be the actuall callsite symbol entry. */
          ToSym = CallSiteSym;
        }
      }
      fout << SymOrdinalF(*FromSym) << " " << SymOrdinalF(*ToSym) << " "
           << CountF(Cnt);
      if ((ToSym->BBTag &&
           ToSym->ContainingFunc->Addr == AdjustedTo) ||
          (!ToSym->BBTag && ToSym->isFunction() &&
           ToSym->Addr == AdjustedTo)) {
        fout << " C";
      } else if (AdjustedTo > ToSym->Addr) {
        // Transfer to the middle of a basic block, usually a return, either a
        // normal one or a return from recursive call, but could it be a dynamic
        // jump?
        fout << " R";
      }
      fout << std::endl;
      ++this->BranchesWritten;
    }
  }
}

void PropellerProfWriter::writeFallthroughs(std::ofstream &fout) {
  for (auto &CA : FallthroughCounters) {
    const uint64_t Cnt = CA.second;
    auto *FromSym = this->findSymbolAtAddress(CA.first.first);
    auto *ToSym = this->findSymbolAtAddress(CA.first.second);
    if (FromSym && ToSym) {
      CountersBySymbol[std::make_pair(FromSym, ToSym)] += Cnt;
    }
  }

  fout << "Fallthroughs" << std::endl;
  for (auto &FC : CountersBySymbol)
    fout << SymOrdinalF(*(FC.first.first)) << " "
         << SymOrdinalF(*(FC.first.second)) << " " << CountF(FC.second)
         << std::endl;
  this->FallthroughsWritten = CountersBySymbol.size();
}

bool PropellerProfWriter::initBinaryFile() {
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

bool PropellerProfWriter::populateSymbolMap() {
  auto Symbols = ObjectFile->symbols();
  for (const auto &Sym : Symbols) {
    auto AddrR = Sym.getAddress();
    auto SecR = Sym.getSection();
    auto NameR = Sym.getName();
    auto TypeR = Sym.getType();

    if (!(AddrR && *AddrR && SecR && (*SecR)->isText() && NameR && TypeR))
      continue;

    StringRef Name = *NameR;
    if (Name.empty()) continue;
    uint64_t Addr = *AddrR;
    uint8_t Type(*TypeR);
    llvm::object::ELFSymbolRef ELFSym(Sym);
    uint64_t Size = ELFSym.getSize();
    auto &L = AddrMap[Addr];
    if (!L.empty()) {
      // If we already have a symbol at the same address with same size, merge
      // them together.
      SymbolEntry *SymbolIsAliasedWith = nullptr;
      for (auto *S : L) {
        if (S->Size == Size) {
          // Make sure Name and Aliased name are both BB or both NON-BB.
          if (SymbolEntry::isBBSymbol(S->Name) !=
              SymbolEntry::isBBSymbol(Name)) {
            continue;
          }
          S->Aliases.push_back(Name);
          if (!S->isFunction() &&
              Type == llvm::object::SymbolRef::ST_Function) {
            // If any of the aliased symbols is a function, promote the whole
            // group to function.
            S->Type = llvm::object::SymbolRef::ST_Function;
          }
          SymbolIsAliasedWith = S;
          break;
        }
      }
      if (SymbolIsAliasedWith) continue;
    }

    SymbolEntry *NewSymbolEntry =
        new SymbolEntry(0, Name, SymbolEntry::AliasesTy(), Addr, Size, Type);
    SymbolList.emplace_back(NewSymbolEntry);
    L.push_back(NewSymbolEntry);
    NewSymbolEntry->BBTag = SymbolEntry::isBBSymbol(Name);
  }  // End of iterating all symbols.

  // Now scan all the symbols in address order to create function <-> bb
  // relationship.
  decltype (AddrMap)::iterator LastFuncPos = AddrMap.end();
  for (auto P = AddrMap.begin(), Q = AddrMap.end(); P != Q; ++P) {
    bool debug = (P->first == 0x000000083c867d);
    for (auto *S : P->second) {
      if (S->isFunction() && !S->BBTag) {
        LastFuncPos = P;
        break;
      }
    }
    if (LastFuncPos == AddrMap.end())  continue;
    for (auto *S : P->second) {
      if (!S->BBTag){
        S->ContainingFunc = S;
        continue;
      }
      // this is a bb symbol, find a wrapping func for it.
      SymbolEntry *ContainingFunc = nullptr;
      for (SymbolEntry *FP : LastFuncPos->second) {
        if (FP->isFunction() && !FP->BBTag &&
            /*  FP->containsAnotherSymbol(S) && */ FP->isFunctionForBBName(S->Name)) {
          if (ContainingFunc == nullptr) {
            ContainingFunc = FP;
          } else {
            // Already has a containing function, so we have at least 2
            // different functions with different sizes but start at the same
            // address, impossible.
            LOG(ERROR) << "Analyzing failure: at address 0x" << hex
                       << LastFuncPos->first
                       << ", there are 2 different functions: "
                       << ContainingFunc->Name.str() << " and "
                       << FP->Name.str();
            return false;
          }
        }
      }
      if (!ContainingFunc) {
        // Disambiguate the following case:
        // 0x10 foo       size = 2
        // 0x12 foo.bb.1  size = 2
        // 0x14 foo.bb.2  size = 0
        // 0x14 bar  <- LastFuncPos set is to bar.
        // 0x14 bar.bb.1
        // In this scenario, we seek lower address.
        auto T = LastFuncPos;
        while (T != AddrMap.begin()) {
          T = std::prev(T);
          for (auto *KS : T->second) {
            if (KS->isFunction() && !KS->BBTag &&
                KS->containsAnotherSymbol(S) &&
                KS->isFunctionForBBName(S->Name)) {
              ContainingFunc = KS;
              break;
            }
          }
        }
      }
      S->ContainingFunc = ContainingFunc;
      if (S->ContainingFunc == nullptr) {
        LOG(ERROR) << "Warning: failed to find function for bb symbol, symbol "
                      "is dropped: "
                   << S->Name.str() << " @ 0x" << hex << S->Addr;
        AddrMap.erase(P--);
        break;
      } else {
        if (!ContainingFunc->isFunctionForBBName(S->Name)) {
          LOG(ERROR) << "Internal check warning: \n"
                     << "Sym: " << S->Name.str() << "\n"
                     << "Func: " << S->ContainingFunc->Name.str();
          return false;
        }
      }
      // Replace the whole name (e.g. "aaaa.BB.foo" with "aaaa" only);
      StringRef BName;
      SymbolEntry::isBBSymbol(S->Name, nullptr, &BName);
      S->Name = BName;
    }  // End of iterating P->second
  }  // End of iterating AddrMap.
  return true;
}

bool PropellerProfWriter::parsePerfData() {
  this->PerfDataFileParsed = 0;
  StringRef FN(PerfFileName);
  while (!FN.empty()) {
    StringRef PerfName;
    std::tie(PerfName, FN) = FN.split(',');
    if (!parsePerfData(PerfName.str())) {
      return false;
    }
    ++this->PerfDataFileParsed;
  }
  LOG(INFO) << "Processed " << this->PerfDataFileParsed << " perf file(s).";
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
    LOG(ERROR) << "Failed to parse perf raw events for perf file: " << PName;
    return false;
  }

  if (!FLAGS_ignore_build_id) {
    if (!setupBinaryMMapName(PR, PName)) {
      return false;
    }
  }

  if (!setupMMaps(Parser, PName)) {
    LOG(ERROR) << "Failed to find perf mmaps for binary '" << BinaryFileName
               << "'.";
    return false;
  }
 
  aggregateLBR(Parser);
  return true;
}

bool PropellerProfWriter::setupMMaps(quipper::PerfParser &Parser,
                                     const string &PName) {
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
  } CompFunc(FLAGS_match_mmap_file.empty()
                 ? (this->BinaryMMapName.empty() ? BinaryFileName
                                                 : this->BinaryMMapName)
                 : FLAGS_match_mmap_file);

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
                 << "' in the perf data file '" << PName
                 << "'. Consider using absolute "
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
                 << hex << LoadAddr + LoadSize << ") in '" << PName << "'.";
      return false;
    }
  }  // End of iterating mmap events.

  if (BinaryMMaps.empty()) {
    LOG(ERROR) << "Failed to find '" << PName << "' mmap entry for '"
               << BinaryFileName << "'. ";
    return false;
  }
  for (auto &M : BinaryMMaps) {
    LOG(INFO) << "Found '" << PName << "' mmap for binary: '"
              << BinaryFileName << "', start mapping address=" << hex << M.first
              << ", mapped size=" << M.second << ".";
  }
  return true;
}

bool PropellerProfWriter::setupBinaryMMapName(quipper::PerfReader &PR,
                                              const string &PName) {
  this->BinaryMMapName = "";
  if (FLAGS_ignore_build_id || this->BinaryBuildId.empty()) {
    return true;
  }
  list<pair<string, string>> ExistingBuildIds;
  for (const auto &BuildId : PR.build_ids()) {
    if (BuildId.has_filename() && BuildId.has_build_id_hash()) {
      string PerfBuildId = BuildId.build_id_hash();
      quipper::PerfizeBuildIDString(&PerfBuildId);
      ExistingBuildIds.emplace_back(BuildId.filename(), PerfBuildId);
      if (PerfBuildId == this->BinaryBuildId) {
        this->BinaryMMapName = BuildId.filename();
        LOG(INFO) << "Found file with matching BuildId in perf file '" << PName
                  << "': " << this->BinaryMMapName;
        return true;
      }
    }
  }
  LOG(INFO) << "No file with matching BuildId in perf data '" << PName
            << "', which contains the following <file, buildid>:";
  for (auto &P : ExistingBuildIds) {
    LOG(INFO) << "\t" << P.first << ": " << BuildIdWrapper(P.second.c_str());
  }
  return false;
}

void PropellerProfWriter::aggregateLBR(quipper::PerfParser &Parser) {
  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() ==
        quipper::PerfDataProto_PerfEvent::kSampleEvent) {
      auto BRStack = EPtr->sample_event().branch_stack();
      if (BRStack.empty())
        continue;
      uint64_t LastFrom = INVALID_ADDRESS;
      uint64_t LastTo = INVALID_ADDRESS;
      for (int P = BRStack.size() - 1; P >= 0; --P) {
        const auto &BE = BRStack.Get(P);
        uint64_t From = BE.from_ip();
        uint64_t To = BE.to_ip();
        if (P == 0 && From == LastFrom && To == LastTo) {
          // LOG(INFO) << "Ignoring duplicate LBR entry: 0x" << std::hex << From
          //           << "-> 0x" << To << std::dec << "\n";
          continue;
        }
        ++(BranchCounters[std::make_pair(From, To)]);
        if (LastTo != INVALID_ADDRESS && LastTo <= From) {
          ++(FallthroughCounters[std::make_pair(LastTo, From)]);
        }
        LastTo = To;
        LastFrom = From;
      }
    }
  }
}

bool PropellerProfWriter::findBinaryBuildId() {
  this->BinaryBuildId = "";
  if (FLAGS_ignore_build_id)
    return true;
  bool BuildIdFound = false;
  for (auto &SR : ObjectFile->sections()) {
    llvm::object::ELFSectionRef ESR(SR);
    StringRef SName;
    auto ExpectedSContents = SR.getContents();
    if (ESR.getType() == llvm::ELF::SHT_NOTE &&
        SR.getName(SName).value() == 0 && SName == ".note.gnu.build-id" &&
        ExpectedSContents && !ExpectedSContents->empty()) {
      StringRef SContents = *ExpectedSContents;
      const unsigned char *P = SContents.bytes_begin() + 0x10;
      if (P >= SContents.bytes_end()) {
        LOG(INFO) << "Section '.note.gnu.build-id' does not contain valid "
                      "build id information.";
        return true;
      }
      string BuildId((const char *)P, SContents.size() - 0x10);
      quipper::PerfizeBuildIDString(&BuildId);
      this->BinaryBuildId = BuildId;
      LOG(INFO) << "Found Build Id in binary '" << BinaryFileName
                << "': " << BuildIdWrapper(BuildId.c_str());
      return true;
    }
  }
  LOG(INFO) << "No Build Id found in '" << this->BinaryFileName << "'.";
  return true;  // always returns true
}

#endif
