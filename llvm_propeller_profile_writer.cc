#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_propeller_profile_writer.h"

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

using llvm::dyn_cast;
using llvm::object::ELFFile;
using llvm::object::ELFObjectFileBase;
using llvm::object::ELFObjectFile;
using llvm::object::ObjectFile;
using llvm::StringRef;
using std::list;
using std::ofstream;
using std::pair;
using std::string;
using std::stringstream;
using std::tuple;
using std::make_pair;

PropellerProfWriter::PropellerProfWriter(const string &BFN, const string &PFN,
                                         const string &OFN)
    : BinaryFileName(BFN), PerfFileName(PFN), PropOutFileName(OFN) {}

PropellerProfWriter::~PropellerProfWriter() {}

SymbolEntry *
PropellerProfWriter::findSymbolAtAddress(uint64_t Pid, uint64_t OriginAddr) {
  uint64_t Addr = adjustAddressForPIE(Pid, OriginAddr);
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

struct Hex0xOut {
} hex0x;

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

struct BuildIdWrapper {
  BuildIdWrapper(const quipper::PerfDataProto_PerfBuildID &BuildId)
      : Data(BuildId.build_id_hash().c_str()) {}
  
  BuildIdWrapper(const char *P) : Data(P) {}
    
  const char *Data;
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

static std::ostream &operator<<(std::ostream &OS, const MMapEntry &ME) {
  return OS << "[" << hex0x << ME.LoadAddr << ", " << hex0x << ME.getEndAddr()
            << "] (PgOff=" << hex0x << ME.PageOffset << ", Size=" << hex0x
            << ME.LoadSize << ")";
};

static std::ostream &operator<<(std::ostream &out, const BuildIdWrapper &BW) {
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

  writeOuts(fout);
  writeSymbols(fout);
  writeBranches(fout);
  writeFallthroughs(fout);
  writeFuncList(fout);
  LOG(INFO) << "Wrote propeller profile (" << this->PerfDataFileParsed
            << " file(s), " << this->SymbolsWritten << " syms, "
            << this->BranchesWritten << " branches, "
            << this->FallthroughsWritten << " fallthroughs) to "
            << PropOutFileName;

  uint64_t TotalBBsWithinFuncsWithProf = 0;
  for (auto &SE: FuncsWithProf) {
    auto I = this->SymbolNameMap.find(SE);
    if (I != this->SymbolNameMap.end()) {
      auto *SEPtr = I->second.get();
      if (!SEPtr->BBTag) {
        TotalBBsWithinFuncsWithProf += this->FuncBBCounter[SEPtr->Ordinal];
      }
    }
  }
  uint64_t TotalFuncs = 0;
  uint64_t TotalBBsAll = 0;
  for (auto &P : this->SymbolNameMap) {
    SymbolEntry *S = P.second.get();
    if (S->BBTag) {
      ++TotalBBsAll;
    } else {
      ++TotalFuncs;
    }
  }
  uint64_t BBsWithProf = this->BBsWithProf.size();
  LOG(INFO) << "Total funs: " << TotalFuncs
            << ", total funcs w/ prof: " << FuncsWithProf.size()
            << ", total BBs: " << TotalBBsAll
            << ", total BBs within hot funcs: "
            << TotalBBsWithinFuncsWithProf
            << ", total BBs with prof: " << BBsWithProf;
  return true;
}

void PropellerProfWriter::writeOuts(ofstream &fout) {
  for (const auto &v :
       set<string>{FLAGS_match_mmap_file, BinaryMMapName, BinaryFileName}) {
    if (!v.empty()) {
      fout << "@" << llvm::sys::path::filename(v).str() << std::endl;
    }
  }
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
        ++this->FuncBBCounter[SE.ContainingFunc->Ordinal];
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
    if (S->BBTag) {
      this->BBsWithProf.insert(S->Ordinal);
    }
  };

  using BrCntSummationKey = tuple<SymbolEntry *, SymbolEntry *, char>;
  struct BrCntSummationKeyComp {
    bool operator()(const BrCntSummationKey &K1,
                    const BrCntSummationKey &K2) const {
      SymbolEntry *From1, *From2, *To1, *To2;
      char T1, T2;
      std::tie(From1, To1, T1) = K1;
      std::tie(From2, To2, T2) = K2;
      if (From1->Ordinal != From2->Ordinal)
        return From1->Ordinal < From2->Ordinal;
      if (To1->Ordinal != To2->Ordinal)
        return To1->Ordinal < To2->Ordinal;
      return T1 < T2;
    }
  };
  using BrCntSummationTy =
      map<BrCntSummationKey, uint64_t, BrCntSummationKeyComp>;
  BrCntSummationTy BrCntSummation;

  for (auto &BCPid: BranchCountersByPid) {
    const uint64_t Pid = BCPid.first;
    auto &BC = BCPid.second;
    for (auto &EC : BC) {
      const uint64_t From = EC.first.first;
      const uint64_t To = EC.first.second;
      const uint64_t Cnt = EC.second;
      auto *FromSym = findSymbolAtAddress(Pid, From);
      auto *ToSym = findSymbolAtAddress(Pid, To);
      const uint64_t AdjustedTo = adjustAddressForPIE(Pid, To);
      
      recordFuncsWithProf(FromSym);
      recordFuncsWithProf(ToSym);
      if (FromSym && ToSym) {
        /* If a return jumps to an address associated with a BB symbol ToSym,
         * then find the actual callsite symbol which is the symbol right
         * before ToSym. */
        if (ToSym->BBTag &&
            (FromSym->ContainingFunc->Addr != ToSym->ContainingFunc->Addr) &&
            ToSym->ContainingFunc->Addr != AdjustedTo &&
            AdjustedTo == ToSym->Addr) { /* implies an inter-procedural return
                                            to the end of a basic block */
          auto *CallSiteSym = findSymbolAtAddress(Pid, To - 1);
          // LOG(INFO) << std::hex << "Return From: 0x" << From << " To: 0x" <<
          // To
          //           << " Callsite symbol: 0x"
          //           << (CallSiteSym ? CallSiteSym->Addr : 0x0) << "\n"
          //           << std::dec;
          if (CallSiteSym && CallSiteSym->BBTag) {
            /* Account for the fall-through between CallSiteSym and ToSym. */
            CountersBySymbol[make_pair(CallSiteSym, ToSym)] += Cnt;
            /* Reassign ToSym to be the actuall callsite symbol entry. */
            ToSym = CallSiteSym;
          }
        }

        char Type = ' ';
        if ((ToSym->BBTag && ToSym->ContainingFunc->Addr == AdjustedTo) ||
            (!ToSym->BBTag && ToSym->isFunction() &&
             ToSym->Addr == AdjustedTo)) {
          Type = 'C';
        } else if (AdjustedTo > ToSym->Addr) {
          // Transfer to the middle of a basic block, usually a return, either a
          // normal one or a return from recursive call, but could it be a
          // dynamic jump?
          Type = 'R';
          // fprintf(stderr, "R: From=0x%lx(0x%lx), To=0x%lx(0x%lx), Pid=%ld\n",
          //         From, adjustAddressForPIE(Pid, From), To,
          //         adjustAddressForPIE(Pid, To), Pid);
        }
        BrCntSummation[std::make_tuple(FromSym, ToSym, Type)] += Cnt;
      }
    }
  }

  for (auto &BrEnt : BrCntSummation) {
    SymbolEntry *FromSym, *ToSym;
    char Type;
    std::tie(FromSym, ToSym, Type) = BrEnt.first;
    uint64_t Cnt = BrEnt.second;
    fout << SymOrdinalF(*FromSym) << " " << SymOrdinalF(*ToSym) << " "
         << CountF(Cnt);
    if (Type != ' ') {
      fout << ' ' << Type;
    }
    fout << std::endl;
    ++this->BranchesWritten;
  }
}

void PropellerProfWriter::writeFallthroughs(std::ofstream &fout) {
  for (auto &CAPid : FallthroughCountersByPid) {
    uint64_t Pid = CAPid.first;
    for (auto &CA : CAPid.second) {
      const uint64_t Cnt = CA.second;
      auto *FromSym = this->findSymbolAtAddress(Pid, CA.first.first);
      auto *ToSym = this->findSymbolAtAddress(Pid, CA.first.second);
      if (FromSym && ToSym) {
        CountersBySymbol[std::make_pair(FromSym, ToSym)] += Cnt;
      }
    }
  }

  fout << "Fallthroughs" << std::endl;
  for (auto &FC : CountersBySymbol)
    fout << SymOrdinalF(*(FC.first.first)) << " "
         << SymOrdinalF(*(FC.first.second)) << " " << CountF(FC.second)
         << std::endl;
  this->FallthroughsWritten = CountersBySymbol.size();
}

template <class ELFT>
bool fillELFPhdr(llvm::object::ELFObjectFileBase *EBFile,
                 map<uint64_t, uint64_t> &PhdrLoadMap) {
  ELFObjectFile<ELFT> *eobj = dyn_cast<ELFObjectFile<ELFT>>(EBFile);
  if (!eobj) return false;
  const ELFFile<ELFT> *efile = eobj->getELFFile();
  if (!efile) return false;
  auto program_headers = efile->program_headers();
  if (!program_headers) return false;
  for (const typename ELFT::Phdr &phdr : *program_headers) {
    if (phdr.p_type == llvm::ELF::PT_LOAD &&
        (phdr.p_flags & llvm::ELF::PF_X) != 0) {
      auto E = PhdrLoadMap.find(phdr.p_vaddr);
      if (E == PhdrLoadMap.end()) {
        PhdrLoadMap.emplace(phdr.p_vaddr, phdr.p_memsz);
      } else {
        if (E->second != phdr.p_memsz) {
          LOG(ERROR) << "Invalid phdr found in elf binary file.";
          return false;
        }
      }
    }
  }
  if (PhdrLoadMap.empty()) {
    LOG(ERROR) << "No loadable and executable segments found in binary.";
    return false;
  }
  stringstream SS;
  SS << "Loadable and executable segments:\n";
  for (auto &Seg: PhdrLoadMap) {
    SS << "\tvaddr=" << hex0x << Seg.first << ", memsz=" << hex0x << Seg.second
       << std::endl;
  }
  LOG(INFO) << SS.str();
  return true;
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
  this->ObjFile = std::move(*ObjOrError);

  auto *ELFObjBase = dyn_cast<ELFObjectFileBase, ObjectFile>(ObjFile.get());
  BinaryIsPIE = (ELFObjBase->getEType() == llvm::ELF::ET_DYN);
  if (BinaryIsPIE) {
    const char *ELFIdent = BinaryFileContent->getBufferStart();
    const char ELFClass = ELFIdent[4];
    const char ELFData = ELFIdent[5];
    if (ELFClass == 1 && ELFData == 1) {
      fillELFPhdr<llvm::object::ELF32LE>(ELFObjBase, PhdrLoadMap);
    } else if (ELFClass == 1 && ELFData == 2) {
      fillELFPhdr<llvm::object::ELF32BE>(ELFObjBase, PhdrLoadMap);
    } else if (ELFClass == 2 && ELFData == 1) {
      fillELFPhdr<llvm::object::ELF64LE>(ELFObjBase, PhdrLoadMap);
    } else if (ELFClass == 2 && ELFData == 2) {
      fillELFPhdr<llvm::object::ELF64BE>(ELFObjBase, PhdrLoadMap);
    } else {
      assert(false);
    }
  }
  LOG(INFO) << "'" << this->BinaryFileName
               << "' is PIE binary: " << BinaryIsPIE;
  return true;
}

bool PropellerProfWriter::populateSymbolMap() {
  auto Symbols = ObjFile->symbols();
  const set<StringRef> ExcludedSymbols{"__cxx_global_array_dtor"};
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

    StringRef BBFunctionName;
    bool isFunction = (Type == llvm::object::SymbolRef::ST_Function);
    bool isBB = SymbolEntry::isBBSymbol(Name, &BBFunctionName);

    if (!isFunction && !isBB) continue;
    if (ExcludedSymbols.find(isBB ? BBFunctionName : Name) !=
        ExcludedSymbols.end()) {
      continue;
    }

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

    auto ExistingNameR = SymbolNameMap.find(Name);
    if (ExistingNameR != SymbolNameMap.end()) {
      // LOG(INFO) << "Dropped duplicate symbol \"" << Name.str() << "\". "
      //           << "Consider using \"-funique-internal-funcnames\" to "
      //              "dedupe internal function names.";
      auto ExistingLI = AddrMap.find(ExistingNameR->second->Addr);
      if (ExistingLI != AddrMap.end()) {
        ExistingLI->second.remove_if(
            [&Name](SymbolEntry *S) { return S->Name == Name; });
      }
      SymbolNameMap.erase(ExistingNameR);
      continue;
    }

    SymbolEntry *NewSymbolEntry =
        new SymbolEntry(0, Name, SymbolEntry::AliasesTy(), Addr, Size, Type);
    L.push_back(NewSymbolEntry);
    NewSymbolEntry->BBTag = SymbolEntry::isBBSymbol(Name);
    SymbolNameMap.emplace(std::piecewise_construct, std::forward_as_tuple(Name),
                          std::forward_as_tuple(NewSymbolEntry));
  }  // End of iterating all symbols.

  // Now scan all the symbols in address order to create function <-> bb
  // relationship.
  uint64_t BBSymbolDropped = 0;
  decltype (AddrMap)::iterator LastFuncPos = AddrMap.end();
  for (auto P = AddrMap.begin(), Q = AddrMap.end(); P != Q; ++P) {
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
            FP->containsAnotherSymbol(S) &&
            FP->isFunctionForBBName(S->Name)) {
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
        int FunctionSymbolSeen = 0;
        while (T != AddrMap.begin()) {
          T = std::prev(T);
          bool isFunction = false;
          for (auto *KS : T->second) {
            isFunction |= KS->isFunction();
            if (KS->isFunction() && !KS->BBTag &&
                KS->containsAnotherSymbol(S) &&
                KS->isFunctionForBBName(S->Name)) {
              ContainingFunc = KS;
              break;
            }
          }
          FunctionSymbolSeen += isFunction ? 1 : 0;
          // Only go back for at most 2 function symbols.
          if (FunctionSymbolSeen > 2) break;
        }
      }
      S->ContainingFunc = ContainingFunc;
      if (S->ContainingFunc == nullptr) {
        LOG(ERROR) << "Dropped bb symbol without any wrapping function: \""
                   << S->Name.str() << "\" @ " << hex0x << S->Addr;
        ++BBSymbolDropped;
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
  if (BBSymbolDropped) {
    LOG(INFO) << "Dropped " << dec << BBSymbolDropped << " bb symbol(s).";
  }
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
    if (!CompFunc(MMapFileName) || !MMap.has_start() || !MMap.has_len() ||
        !MMap.has_pid())
      continue;

    if (this->BinaryMMapName.empty()) {
      this->BinaryMMapName = MMapFileName;
    } else if (BinaryMMapName != MMapFileName) {
      LOG(ERROR) << "'" << BinaryFileName
                 << "' is not specific enough. It matches both '"
                 << BinaryMMapName << "' and '" << MMapFileName
                 << "' in the perf data file '" << PName
                 << "'. Consider using absolute file name.";
      return false;
    }
    uint64_t LoadAddr = MMap.start();
    uint64_t LoadSize = MMap.len();
    uint64_t PageOffset = MMap.has_pgoff() ? MMap.pgoff() : 0;

    // For the same binary, MMap can only be different if it is a PIE binary. So
    // for non-PIE binaries, we check all MMaps are equal and merge them into
    // BinaryMMapByPid[0].
    uint64_t MPid = BinaryIsPIE ? MMap.pid() : 0;
    set<MMapEntry> &LoadMap = BinaryMMapByPid[MPid];
    // Check for mmap conflicts.
    if (!checkBinaryMMapConflictionAndEmplace(LoadAddr, LoadSize, PageOffset,
                                              LoadMap)) {
      stringstream SS;
      SS << "Found conflict MMap event: "
         << MMapEntry{LoadAddr, LoadSize, PageOffset}
         << ". Existing MMap entries: " << std::endl;
      for (auto &EM : LoadMap) {
        SS << "\t" << EM << std::endl;
      }
      LOG(ERROR) << SS.str();
      return false;
    }
  }  // End of iterating mmap events.

  if (!std::accumulate(
          BinaryMMapByPid.begin(), BinaryMMapByPid.end(), 0,
          [](uint64_t V, const decltype(BinaryMMapByPid)::value_type &S)
              -> uint64_t { return V + S.second.size(); })) {
    LOG(ERROR) << "Failed to find MMap entries in '" << PName << "' for '"
               << BinaryFileName << "'.";
    return false;
  }
  for (auto &M : BinaryMMapByPid) {
    stringstream SS;
    SS << "Found mmap in '" << PName << "' for binary: '" << BinaryFileName
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
    uint64_t LoadAddr, uint64_t LoadSize, uint64_t PageOffset,
    set<MMapEntry> &M) {
  for (const MMapEntry &E : M) {
    if (E.LoadAddr == LoadAddr && E.LoadSize == LoadSize &&
        E.PageOffset == PageOffset)
      return true;
    if (!((LoadAddr + LoadSize <= E.LoadAddr) ||
          (E.LoadAddr + E.LoadSize <= LoadAddr)))
      return false;
  }
  auto R = M.emplace(LoadAddr, LoadSize, PageOffset);
  assert(R.second);
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
  stringstream SS;
  SS << "No file with matching BuildId in perf data '" << PName
     << "', which contains the following <file, buildid>:" << std::endl;
  for (auto &P : ExistingBuildIds) {
    SS << "\t" << P.first << ": " << BuildIdWrapper(P.second.c_str())
       << std::endl;
  }
  LOG(INFO) << SS.str();
  return false;
}

void PropellerProfWriter::aggregateLBR(quipper::PerfParser &Parser) {
  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() ==
        quipper::PerfDataProto_PerfEvent::kSampleEvent) {
      auto &SEvent = EPtr->sample_event();
      if (!SEvent.has_pid()) continue;
      auto BRStack = SEvent.branch_stack();
      if (BRStack.empty())
        continue;
      uint64_t Pid = BinaryIsPIE ? SEvent.pid() : 0;
      auto &BranchCounters = BranchCountersByPid[Pid];
      auto &FallthroughCounters = FallthroughCountersByPid[Pid];
      if (BinaryMMapByPid.find(Pid) == BinaryMMapByPid.end()) continue;
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
        ++(BranchCounters[make_pair(From, To)]);
        if (LastTo != INVALID_ADDRESS && LastTo <= From) {
          ++(FallthroughCounters[make_pair(LastTo, From)]);
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
  for (auto &SR : ObjFile->sections()) {
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
