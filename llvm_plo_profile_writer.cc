#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_plo_profile_writer.h"

#include <fstream>
#include <functional>
#include <ios>
#include <list>
#include <string>
#include <utility>

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include "third_party/perf_data_converter/src/quipper/perf_parser.h"

using llvm::StringRef;
using std::string;

PLOProfileWriter::PLOProfileWriter(const string &BFN, const string &PFN,
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

  // If it is within binary mmap.
  // BinaryMMaps usually contains 1-2 elements at most.
  uint64_t LoadAddr = findLoadAddress(OriginAddr);
  if (LoadAddr == INVALID_ADDRESS) return Addr2SymCache.end();

  uint64_t Addr = adjustAddressForPIE(OriginAddr);

  auto U = AddrMap.upper_bound(Addr);
  if (U == AddrMap.begin()) return Addr2SymCache.end();
  auto R = std::prev(U);

  std::list<SymbolEntry *> Candidates;
  for (auto &SymEnt : R->second)
    if (SymEnt->conainsAddress(Addr))
      Candidates.push_back(SymEnt.get());

  if (Candidates.empty()) return Addr2SymCache.end();

  // Sort candidates by symbol size.
  Candidates.sort([](const SymbolEntry *S1, const SymbolEntry *S2) {
    return S1->Size < S2->Size;
  });

  if (Candidates.size() == 2) {
    // If one of them is Func, the other is like Func.bb.1,
    // return Func.bb.1.
    auto *Sym = *(Candidates.begin());
    StringRef FuncName;
    if (isBBSymbol(Sym->Name, FuncName) &&
        FuncName == (*(Candidates.rbegin()))->Name) {
      Candidates.pop_back();
    }
  }

  auto InsertR = Addr2SymCache.emplace(OriginAddr, std::move(Candidates));
  assert(InsertR.second);
  return InsertR.first;
}

bool PLOProfileWriter::write() {
  if (!initBinaryFile() || !populateSymbolMap() || !parsePerfData()) {
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

static std::ostream & hexout(std::ostream &out) {
    out.setf(std::ios_base::hex, std::ios_base::basefield);
    out.unsetf(std::ios_base::showbase);
    return out;
}

static std::ostream & decout(std::ostream &out) {
    out.setf(std::ios_base::dec, std::ios_base::basefield);
    out.unsetf(std::ios_base::showbase);
    return out;
}

void PLOProfileWriter::writeSymbols(std::ofstream &fout) {
  fout << "Symbols" << std::endl;
  for (auto &LE : AddrMap) {
    for (auto &SEPtr : LE.second) {
      SymbolEntry &SE = *SEPtr;
      decout(fout) << SE.Ordinal << " ";
      hexout(fout) << SE.Addr << " " << SE.Size << " ";
      if (SE.BBFuncName.empty()) {
        fout << "N" << SE.Name.str() << std::endl;
      } else {
        auto R = NameMap.find(SE.BBFuncName);
        assert(R != NameMap.end());
        SymbolEntry *BBFuncSym = R->second;
        decout(fout) << BBFuncSym->Ordinal << "." << SE.getBBIndex().str()
                     << std::endl;
      }
    }
  }
}

void PLOProfileWriter::writeBranches(std::ofstream &fout) {
  fout << "Branches" << std::endl;
  auto writeSymbolList =
      [&fout](std::list<SymbolEntry *> &Syms) -> std::ostream & {
    assert(!Syms.empty());
    for (auto *P : Syms) {
      if (P != Syms.front()) {
        fout << "/";
      }
      decout(fout) << P->Ordinal;
    }
    return fout;
  };
  for (auto &EC : EdgeCounters) {
    const uint64_t From = EC.first.first;
    const uint64_t To = EC.first.second;
    const uint64_t Cnt = EC.second;
    auto FromSymHandler = findSymbolEntry(From);
    auto ToSymHandler = findSymbolEntry(To);
    if (FromSymHandler != Addr2SymCache.end() &&
        ToSymHandler != Addr2SymCache.end()) {
      writeSymbolList(FromSymHandler->second) << " ";
      writeSymbolList(ToSymHandler->second) << " " ;
      decout(fout) << Cnt << " ";
      hexout(fout) << adjustAddressForPIE(From) << " ";
      hexout(fout) << adjustAddressForPIE(To) << std::endl;
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

  aggregateLBR(Parser);
  return true;
}

bool PLOProfileWriter::setupMMaps(quipper::PerfParser &Parser){
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

  for (const auto &PE : Parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *EPtr = PE.event_ptr;
    if (EPtr->event_type_case() ==
        quipper::PerfDataProto_PerfEvent::kMmapEvent) {
      const quipper::PerfDataProto_MMapEvent &MMap = EPtr->mmap_event();
      if (MMap.has_filename()) {
        const string &MMapFileName = MMap.filename();
        if (CompFunc(MMapFileName) && MMap.has_start() && MMap.has_len()) {
          uint64_t LoadAddr = MMap.start();
          uint64_t LoadSize = MMap.len();
          auto ExistingMMap = BinaryMMaps.find(LoadAddr);
          if (ExistingMMap == BinaryMMaps.end()) {
            BinaryMMaps.emplace(LoadAddr, LoadSize);
          } else if (ExistingMMap->second != LoadSize) {
            LOG(ERROR) << "Binary is loaded into memory segment "
                          "with same starting address but different "
                          "size.";
            return false;
          }
        }
      }
    }
  }
  if (BinaryMMaps.empty()) {
    LOG(ERROR) << "Failed to find mmap entry for '" << BinaryFileName << "'. '"
               << PerfFileName << "' does not match '" << BinaryFileName
               << "'.";
    return false;
  }
  for (auto &M : BinaryMMaps) {
    LOG(INFO) << "Find mmap for binary: '" << BinaryFileName
              << "', start mapping address=" << std::hex << std::showbase
              << M.first << ", mapped size=" << M.second << ".";
  }
  return true;
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
