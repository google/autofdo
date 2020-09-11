#include "config.h"
#if defined(HAVE_LLVM)
#include "llvm_propeller_flags.h"
#include "llvm_propeller_profile_writer.h"
#include "llvm_propeller_profile_format.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

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
#include <vector>

#include "gflags/gflags.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include "third_party/perf_data_converter/src/quipper/perf_parser.h"
#include "third_party/perf_data_converter/src/quipper/perf_reader.h"

using llvm::dyn_cast;
using llvm::Expected;
using llvm::StringRef;
using llvm::object::ELFFile;
using llvm::object::ELFObjectFile;
using llvm::object::ELFObjectFileBase;
using llvm::object::ObjectFile;
using std::list;
using std::make_pair;
using std::ofstream;
using std::pair;
using std::string;
using std::stringstream;
using std::tuple;

PropellerProfWriter::PropellerProfWriter(const string &bfn, const string &pfn,
                                         const string &ofn, const string &sofn)
    : binaryFileName(bfn), perfFileName(pfn), propOutFileName(ofn), symOrderFileName(sofn) {}

PropellerProfWriter::~PropellerProfWriter() {}

static bool
containsAddress(SymbolEntry *sym, uint64_t a) {
  return sym->addr <= a && a < sym->addr + sym->size;
}

static bool
containsAnotherSymbol(SymbolEntry *sym, SymbolEntry *O) {
  if (O->size == 0) {
    // Note if O's size is 0, we allow O on the end boundary. For example, if
    // foo.BB.4 is at address 0x10. foo is [0x0, 0x10), we then assume foo
    // contains foo.BB.4.
    return sym->addr <= O->addr && O->addr <= sym->addr + sym->size;
  }
  return containsAddress(sym, O->addr) &&
         containsAddress(sym, O->addr + O->size - 1);
}

SymbolEntry *PropellerProfWriter::findSymbolAtAddress(uint64_t pid,
                                                      uint64_t originAddr) {
  uint64_t addr = adjustAddressForPIE(pid, originAddr);
  if (addr == INVALID_ADDRESS) return nullptr;
  auto u = addrMap.upper_bound(addr);
  if (u == addrMap.begin()) return nullptr;
  auto r = std::prev(u);

  for (auto &SymEnt : r->second)
    if (containsAddress(SymEnt, addr) && SymEnt->isBasicBlock())
      return SymEnt;

  return nullptr;
}

uint64_t PropellerProfWriter::adjustAddressForPIE(uint64_t pid,
                                                  uint64_t addr) const {
  auto i = binaryMMapByPid.find(pid);
  if (i == binaryMMapByPid.end()) return INVALID_ADDRESS;

  const MMapEntry *mmap = nullptr;
  for (const MMapEntry &p : i->second)
    if (p.loadAddr <= addr && addr < p.loadAddr + p.loadSize) mmap = &p;
  if (!mmap) return INVALID_ADDRESS;
  if (binaryIsPIE) {
    uint64_t file_offset = addr - mmap->loadAddr + mmap->pageOffset;
    const ProgSegLoad *segload = nullptr;
    for (const auto &loadable : phdrLoadMap) {
      if (loadable.second.offset <= file_offset &&
          file_offset <= loadable.second.offset + loadable.second.filesz)
        segload = &(loadable.second);
    }
    if (segload) return file_offset - segload->offset + segload->vaddr;
    LOG(WARNING) << std::showbase << std::hex << "Virtual address: " << addr
                 << "'s file offset is: " << file_offset
                 << ", this offset does not sit in any loadable segment.";
    return INVALID_ADDRESS;
  }
  return addr;
}

SymbolEntry *PropellerProfWriter::CreateFuncSymbolEntry(
    uint64_t ordinal, StringRef func_name, SymbolEntry::AliasesTy aliases,
    int func_bb_num, uint64_t address, uint64_t size) {
  if (bb_addr_map_.find(func_name) != bb_addr_map_.end()) {
    LOG(ERROR) << "Found duplicate function symbol '" << func_name.str() << "'@0x"
               << std::hex << address
               << ", drop the symbol and all its bb symbols.";
    return nullptr;
  }
  // Pre-allocate "func_bb_num" nullptrs in bbinfo_map_[func_name].
  bb_addr_map_.emplace(std::piecewise_construct,
                       std::forward_as_tuple(func_name),
                       std::forward_as_tuple(func_bb_num, nullptr));
  // Note, funcsym is not put into bbinfo_map_. It's accessible via
  // bbinfo_map_[func_name].front()->func_ptr.
  SymbolEntry *funcsym = new SymbolEntry(ordinal, func_name, std::move(aliases),
                                         0, address, size, nullptr, 0);
  // Put into address_map_.
  addrMap[address].emplace_back(funcsym);
  return funcsym;
}

SymbolEntry *PropellerProfWriter::CreateBbSymbolEntry(
    uint64_t ordinal, SymbolEntry *parent_func, int bb_index, uint64_t address,
    uint64_t size, uint32_t metadata) {
  CHECK(parent_func);
  BbAddrMapTy::iterator iter = bb_addr_map_.find(parent_func->fname);
  if (iter == bb_addr_map_.end()) {
    LOG(ERROR) << "BB symbol '" << parent_func->fname.str() << "." << bb_index
               << "'@0x" << std::hex << address
               << " appears before its function record, drop the symbol.";
    return nullptr;
  }
  // Note - bbsym does not have a field for bbindex, because the name field in
  // SymbolEntry is StringRef and in bbinfo workflow, the bbsym's name field is
  // always "". But bbsym's id number can be calculated from: (bbsym->ordinal -
  // bbsym->func_ptr->ordinal - 1).
  SymbolEntry *bb_symbol =
      new SymbolEntry(ordinal, "", SymbolEntry::AliasesTy(), bb_index, address,
                      size, parent_func, metadata);
  CHECK(bb_index < iter->second.size());
  iter->second[bb_index] = bb_symbol;
  addrMap[address].emplace_back(bb_symbol);
  return bb_symbol;
}

static llvm::Optional<llvm::object::SectionRef> FindBbAddrMapSection(
    const llvm::object::ObjectFile &obj) {
  for (auto sec : obj.sections()) {
    Expected<llvm::StringRef> sn = sec.getName();
    if (sn && (*sn) == ".bb_addr_map") return sec;
  }
  return llvm::None;
}

void PropellerProfWriter::ReadSymbolTable() {
  for (llvm::object::SymbolRef sr : objFile->symbols()) {
    llvm::object::ELFSymbolRef symbol(sr);
    uint8_t stt = symbol.getELFType();
    if (stt != llvm::ELF::STT_FUNC) continue;
    Expected<uint64_t> address = sr.getAddress();
    if (!address || !*address) continue;
    Expected<StringRef> func_name = symbol.getName();
    if (!func_name) continue;

    auto &addr_sym_list = symtab_[*address];
    // Check whether there are already symbols on the same address, if so make
    // sure they have the same size and thus they can be aliased.
    const uint64_t func_size = symbol.getSize();
    bool check_size_ok = true;
    for (auto &sym_ref : addr_sym_list) {
      uint64_t sym_size = llvm::object::ELFSymbolRef(sym_ref).getSize();
      if (func_size != sym_size) {
        LOG(WARNING) << "Multiple function symbols on the same address with "
                        "different size: "
                     << *address << ": '" << func_name->str() << "("
                     << func_size << ")' and '"
                     << llvm::cantFail(sym_ref.getName()).str() << "("
                     << sym_size << ")', the latter will be dropped.";
        check_size_ok = false;
        break;
      }
    }
    if (check_size_ok) addr_sym_list.push_back(sr);
  }
}

// Read a uleb field value.
template <class ValueType>
static bool ReadUlebField(const char *field_name, const uint8_t **p,
                          const unsigned char *end_mark, ValueType *v) {
  const char *err = nullptr;
  unsigned n = 0;
  *v = llvm::decodeULEB128(*p, &n, end_mark, &err);
  if (err != nullptr) {
    LOG(ERROR) << "Read .bb_info '" << field_name << "' uleb128 error: " << err;
    return false;
  }
  *p += n;
  return true;
}

bool PropellerProfWriter::ReadBbAddrMapSection() {
  llvm::Expected<StringRef> exp_contents = bbaddrmap_section_.getContents();
  if (!exp_contents) {
    LOG(ERROR) << "Error accessing .bb_info section content.";
    return false;
  }
  StringRef sec_contents = *exp_contents;
  const unsigned char * p = sec_contents.bytes_begin();
  const unsigned char * const section_end = sec_contents.bytes_end();

  uint64_t ordinal = 0;
  while (p != section_end) {
    const uint64_t func_address = *(reinterpret_cast<const uint64_t *>(p));
    auto iter = symtab_.find(func_address);
    CHECK(iter != symtab_.end());
    SymbolEntry::AliasesTy func_aliases;
    for (llvm::object::SymbolRef &sr : iter->second)
      func_aliases.push_back(llvm::cantFail(sr.getName()));
    CHECK(!func_aliases.empty());

    // fprintf(stderr, "offset: %lu, func_address: 0x%p\n",
    //         p - sec_contents.bytes_begin(), reinterpret_cast<void *>(func_address));

    const uint64_t func_size =
        llvm::object::ELFSymbolRef(iter->second.front()).getSize();
    p += sizeof(uint64_t);
    uint64_t num_blocks = 0, meta = 0, size = 0, offset = 0,
             previous_offset = 0;
    if (!ReadUlebField("bb count", &p, section_end, &num_blocks)) return false;
    CHECK(num_blocks);
    SymbolEntry *func_symbol =
        CreateFuncSymbolEntry(++ordinal, func_aliases.front(), func_aliases,
                              num_blocks, func_address, func_size);
    // Note - func_symbol can be null, we don't bail out until we can't proceed.
    for (int ib = 0; ib < num_blocks; ++ib) {
      previous_offset = offset;
      if (ReadUlebField("bb offset", &p, section_end, &offset) &&
          ReadUlebField("bb size", &p, section_end, &size) &&
          ReadUlebField("bb metadata", &p, section_end, &meta)) {
        // Check assumption here: bb records appear in the order of "symbol
        // offset".
        CHECK(previous_offset <= offset);
        if (func_symbol &&
            !CreateBbSymbolEntry(++ordinal, func_symbol, ib,
                                 func_symbol->addr + offset, size, meta))
          return false;
        // else if func_symbol is nullptr, we read, move ptrs forward but
        // do not create new symbols.
      } else {
        // Fail to read bbinfo section in the middle is irrecoverable.
        return false;
      }
    }
  }  // end of iterating bb info section.

  // // using BbAddrMapTy = std::map<llvm::StringRef, std::vector<SymbolEntry *>>;
  // // BbAddrMapTy bb_addr_map_;
  // for (auto &[unused_funcname, bbs] : bb_addr_map_) {
  //   assert(!bbs.empty());
  //   SymbolEntry *func = bbs[0]->containingFunc;
  //   fprintf(stderr, "func: %s: 0x%lx: %lu\n", func->fname.str().c_str(), func->addr, func->size);
  //   for (auto *bb: bbs) {
  //     assert(bb->isBasicBlock() && bb->containingFunc == func);
  //     fprintf(stderr, "  bb: %u: 0x%lx: %lu\n", bb->bbindex, bb->addr, bb->size);
  //   }
  // }

  return true;
}

bool PropellerProfWriter::write() {
  if (!initBinaryFile() || !findBinaryBuildId())
    return false;

  auto optBbAddrMapSection = FindBbAddrMapSection(*objFile);
  if (!optBbAddrMapSection) {
    LOG(ERROR) << ".bb_addr_map not found in '" << binaryFileName << "'.";
    return false;
  }
  bbaddrmap_section_ = *optBbAddrMapSection;
  ReadSymbolTable();
  ReadBbAddrMapSection();

  for (auto &[unused, bbvec] : bb_addr_map_) {
    assert(!bbvec.empty());
    SymbolEntry *funcsym = bbvec[0]->containingFunc;
    auto *cfg = new ControlFlowGraph(funcsym->fname, funcsym->size, bbvec);
    cfg->forEachNodeRef([this](CFGNode &n) {
      symbolNodeMap.emplace(n.symbol, &n);
    });
    cfgs.emplace(funcsym, cfg);
  }

  {
    ofstream fout(propOutFileName);
    if (fout.bad()) {
      LOG(ERROR) << "Failed to open '" << propOutFileName << "' for writing.";
      return false;
    }

    ofstream sout(symOrderFileName);
    if (sout.bad()) {
      LOG(ERROR) << "Failed to open '" << symOrderFileName << "' for writing.";
      return false;
    }

    if (!parsePerfData())
      return false;
    recordBranches();
    recordFallthroughs();

    for (auto &cfgElem : cfgs) {
      cfgElem.second->calculateNodeFreqs();
      cfgElem.second->coalesceColdNodes();
    }

    std::list<CFGNode *> section_order;
    llvm::propeller::CodeLayout().doOrder(cfgs, section_order);

    for (auto &[funcsym, cfgptr] : cfgs) {
      assert(funcsym);
      assert(cfgptr.get());
      if (!cfgptr->isHot())
        continue;
      fout << "!" << funcsym->fname.str();
      for (size_t i = 1; i < funcsym->aliases.size(); ++i)
        fout << "/" << funcsym->aliases[i].str();
      fout << "\n";
      for (auto &[unused, cluster] : cfgptr->clusters) {
        fout << "!!";
        for (size_t i = 0; i < cluster.size(); ++i) {
          if (i)
            fout << " ";
          CFGNode *node = cluster[i];
          fout << node->getBBIndex();
        }
        fout << "\n";
      }
    }

    for (CFGNode *n : section_order) {
      if (n->isEntryNode()) {
        sout << n->controlFlowGraph->name.str() << "\n";
      } else {
        ControlFlowGraph *cfg = n->controlFlowGraph;
        int found = -1;
        int i = 0;
        for (auto &[unused, cnodes] : cfg->clusters) {
          for (auto *cn : cnodes) {
            if (cn == n) {
              found = i;
              break;
            }
          }
          ++i;
          if (found != -1)
            break;
        }
        if (found == -1) {
          if (n->symbol->isLandingPadBlock())
            sout << n->symbol->containingFunc->fname.str() + ".eh\n";
          else
            sout << n->symbol->containingFunc->fname.str() + ".cold\n";
        } else {
          sout << n->symbol->containingFunc->fname.str() + "." +
                      std::to_string(found)
               << "\n";
        }
      }
    }
  }
  // This must be done after "fout" is closed.
  // if (!reorderSections(partBegin, partEnd, partNew, partNewEnd)) {
  //   LOG(ERROR)
  //       << "Warn: failed to reorder propeller section, performance may suffer.";
  // }
  summarize();
  return true;
}

void PropellerProfWriter::summarize() {
  LOG(INFO) << "Wrote propeller profile (" << perfDataFileParsed << " file(s), "
            << CommaF(symbolsWritten) << " syms, " << CommaF(branchesWritten)
            << " branches, " << CommaF(fallthroughsWritten)
            << " fallthroughs) to " << propOutFileName;

  LOG(INFO) << CommaF(countersNotAddressed) << " of " << CommaF(totalCounters)
            << " branch entries are not mapped ("
            << PercentageF(countersNotAddressed, totalCounters) << ").";

  LOG(INFO) << CommaF(crossFunctionCounters) << " of " << CommaF(totalCounters)
            << " branch entries are cross function (" << std::setprecision(3)
            << PercentageF(crossFunctionCounters, totalCounters) << ").";

  LOG(INFO) << CommaF(fallthroughStartEndInDifferentFuncs) << " of "
            << CommaF(fallthroughCalculationNumber)
            << " fallthroughs are discareded because from and to are from "
               "different functions ("
            << PercentageF(fallthroughStartEndInDifferentFuncs,
                           fallthroughCalculationNumber)
            << ").";

  uint64_t totalBBsWithinFuncsWithProf = 0;
  uint64_t numBBsWithProf = 0;
  set<uint64_t> funcsWithProf;
  for (auto &se : hotSymbols) {
    if (funcsWithProf.insert(se->containingFunc->ordinal).second)
      totalBBsWithinFuncsWithProf += funcBBCounter[se->containingFunc->ordinal];
    if (!se->isFunction())
      ++numBBsWithProf;
  }
  uint64_t totalFuncs = 0;
  uint64_t totalBBsAll = 0;
  for (auto &p : symbolNameMap)
    for (auto &q : p.second)
      if (!q.second->isFunction())
        ++totalBBsAll;
      else
        ++totalFuncs;
  // LOG(INFO) << CommaF(totalFuncs) << " functions, "
  //           << CommaF(funcsWithProf.size()) << " functions with prof ("
  //           << PercentageF(funcsWithProf, totalFuncs) << ")"
  //           << ", " << CommaF(totalBBsAll) << " BBs (average "
  //           << totalBBsAll / totalFuncs << " BBs per func), "
  //           << CommaF(totalBBsWithinFuncsWithProf) << " BBs within hot funcs ("
  //           << PercentageF(totalBBsWithinFuncsWithProf, totalBBsAll) << "), "
  //           << CommaF(numBBsWithProf) << " BBs with prof (include "
  //           << CommaF(extraBBsIncludedInFallthroughs)
  //           << " BBs that are on the path of "
  //              "fallthroughs, total accounted for "
  //           << PercentageF(numBBsWithProf, totalBBsAll) << " of all BBs).";
}

void PropellerProfWriter::recordBranches() {
  this->branchesWritten = 0;
  auto recordHotSymbol = [this](SymbolEntry *sym) {
    if (!sym || !(sym->containingFunc) || sym->containingFunc->fname.empty())
      return;
    assert(sym->isBasicBlock());
    if (sym->isLandingPadBlock())
      // Landing pads are always treated as not hot.
      LOG(WARNING) << "*** HOT LANDING PAD: " << sym->fname.str() << "\t"
                   << sym->containingFunc->fname.str() << "\n";
    else {
      // Dups are properly handled by set.
      hotSymbols.insert(sym);
    }
  };

  using BrCntSummationKey = tuple<SymbolEntry *, SymbolEntry *, char>;
  struct BrCntSummationKeyComp {
    bool operator()(const BrCntSummationKey &k1,
                    const BrCntSummationKey &k2) const {
      SymbolEntry *From1, *From2, *To1, *To2;
      char T1, T2;
      std::tie(From1, To1, T1) = k1;
      std::tie(From2, To2, T2) = k2;
      if (From1->ordinal != From2->ordinal)
        return From1->ordinal < From2->ordinal;
      if (To1->ordinal != To2->ordinal) return To1->ordinal < To2->ordinal;
      return T1 < T2;
    }
  };
  using BrCntSummationTy =
      map<BrCntSummationKey, uint64_t, BrCntSummationKeyComp>;
  BrCntSummationTy brCntSummation;

  totalCounters = 0;
  countersNotAddressed = 0;
  crossFunctionCounters = 0;
  // {pid: {<from, to>: count}}
  for (auto &bcPid : branchCountersByPid) {
    const uint64_t pid = bcPid.first;
    auto &BC = bcPid.second;
    for (auto &EC : BC) {
      const uint64_t from = EC.first.first;
      const uint64_t to = EC.first.second;
      const uint64_t cnt = EC.second;
      auto *fromSym = findSymbolAtAddress(pid, from);
      auto *toSym = findSymbolAtAddress(pid, to);
      const uint64_t adjustedFrom = adjustAddressForPIE(pid, from);
      const uint64_t adjustedTo = adjustAddressForPIE(pid, to);

      recordHotSymbol(fromSym);
      recordHotSymbol(toSym);

      totalCounters += cnt;

      if (!toSym || !fromSym) {
        countersNotAddressed += cnt;
        continue;
      }

      uint64_t adjusted_from = adjustAddressForPIE(pid, from);
      uint64_t adjusted_to = adjustAddressForPIE(pid, to);
      // if (!fromSym->isFunction() && (adjusted_from < fromSym->addr + fromSym->size - 6)) {
      //   LOG(WARNING) << std::showbase << std::hex << "BR from " << from
      //                << " -> " << to << " (" << adjusted_from << " -> "
      //                << adjusted_to << ", pid=" << pid
      //                << "), source address not within last 5 bytes of bb ["
      //                << fromSym->addr << ", " << fromSym->addr + fromSym->size
      //                << ").";
      // }

      // If this is a return to the beginning of a basic block, change the toSym
      // to the basic block just before and add fallthrough between the two
      // symbols.
      // After this code executes, adjustedTo can never be the beginning of a
      // basic block for returns.
      if ((fromSym->isReturnBlock() ||
           toSym->containingFunc->addr != fromSym->containingFunc->addr) &&
          toSym->containingFunc->addr != adjustedTo &&  // Not a call
          // Jump to the beginning of the basicblock
          adjustedTo == toSym->addr) {
        auto prevAddr = std::prev(addrMap.find(adjustedTo))->first;
        auto *callSiteSym =
            findSymbolAtAddress(pid, to - (adjustedTo - prevAddr));
        if (callSiteSym) {
          recordHotSymbol(callSiteSym);
          // Account for the fall-through between callSiteSym and toSym.
          fallthroughCountersBySymbol[make_pair(callSiteSym, toSym)] += cnt;
          // Reassign toSym to be the actuall callsite symbol entry.
          toSym = callSiteSym;
        } else
          LOG(WARNING) << "*** Internal error: Could not find the right "
                          "callSiteSym for : "
                       << adjustedTo;
      }

      if (fromSym->containingFunc != toSym->containingFunc)
        crossFunctionCounters += cnt;

      char type = ' ';
      if (toSym->containingFunc->addr == adjustedTo) {
        type = 'C';
      } else if (adjustedTo != toSym->addr) {
        type = 'R';
      }
      brCntSummation[std::make_tuple(fromSym, toSym, type)] += cnt;
    }
  }

  for (auto &brEnt : brCntSummation) {
    SymbolEntry *fromSym, *toSym;
    char type;
    std::tie(fromSym, toSym, type) = brEnt.first;
    uint64_t cnt = brEnt.second;
    ++this->branchesWritten;

    CFGNode *fromN = symbolNodeMap[fromSym];
    CFGNode *toN = symbolNodeMap[toSym];
    if (!fromN || !toN)
      continue;
    if (fromN->controlFlowGraph == toN->controlFlowGraph)
      fromN->controlFlowGraph->mapBranch(fromN, toN, cnt, type == 'C',
                                         type == 'R');
    else
      fromN->controlFlowGraph->mapCallOut(fromN, toN, 0, cnt, type == 'C',
                                          type == 'R');
  }
}

// Compute fallthrough BBs from "from" -> "to", and place them in "path".
// ("from" and "to" are excluded)
bool PropellerProfWriter::calculateFallthroughBBs(
    SymbolEntry *from, SymbolEntry *to, std::vector<SymbolEntry *> &path) {
  path.clear();
  ++fallthroughCalculationNumber;
  if (from == to) return true;
  if (from->addr > to->addr) {
    LOG(WARNING) << "*** Internal error: fallthrough path start address is "
                  "larger than end address. ***";
    return false;
  }

  if (!from->canFallthrough()) {
    LOG(WARNING) << "*** Skipping non-fallthrough ***" << from->fname.str() ;
    return false;
  }

  auto p = addrMap.find(from->addr), q = addrMap.find(to->addr),
       e = addrMap.end();
  if (p == e || q == e) {
    LOG(FATAL) << "*** Internal error: invalid symbol in fallthrough pair. ***";
    return false;
  }
  q = std::next(q);
  if (from->containingFunc != to->containingFunc) {
    LOG(ERROR) << "fallthrough (" << SymShortF(from) << " -> "
               << SymShortF(to)
               << ") does not start and end within the same faunction.";
    ++fallthroughStartEndInDifferentFuncs;
    return false;
  }
  auto func = from->containingFunc;
  auto i = p;
  for (++i; i != q && i != e; ++i) {
    bool found = false;
    for (auto *se : i->second) {
      assert(se->containingFunc);
      if (se->isFunction()) {
        continue;
      }
      if (se->containingFunc != func)
        break;
      
      if (se == to) {
        found = true;
        break;
      }

      if (!se->canFallthrough()) {
        LOG(WARNING) << "*** skipping non-fallthrough ***" << se->fname.str();
        return false;
      }
      path.emplace_back(se);
      found = true;
    }

    if (!found) {
      LOG(ERROR) << "failed to find a BB for "
                 << "fallthrough (" << SymShortF(*from) << " -> "
                 << SymShortF(to) << ").";
      return false;
    }
  }
  if (path.size() >= 200)
    LOG(ERROR) << "too many BBs along fallthrough (" << SymShortF(from)
               << " -> " << SymShortF(to) << "): " << ::dec << path.size()
               << " BBs.";
  return true;
}

void PropellerProfWriter::recordFallthroughs() {
  // caPid: {pid, <<from_addr, to_addr>, counter>}
  for (auto &caPid : fallthroughCountersByPid) {
    uint64_t pid = caPid.first;
    for (auto &ca : caPid.second) {
      const uint64_t cnt = ca.second;
      auto *fromSym = findSymbolAtAddress(pid, ca.first.first);
      auto *toSym = findSymbolAtAddress(pid, ca.first.second);
      if (fromSym && toSym)
        fallthroughCountersBySymbol[std::make_pair(fromSym, toSym)] += cnt;
    }
  }

  extraBBsIncludedInFallthroughs = 0;
  fallthroughStartEndInDifferentFuncs = 0;
  fallthroughCalculationNumber = 0;
  for (auto &fc : fallthroughCountersBySymbol) {
    std::vector<SymbolEntry *> path;
    SymbolEntry *fallthroughFrom = fc.first.first,
                *fallthroughTo = fc.first.second;
    if (fallthroughFrom != fallthroughTo &&
        calculateFallthroughBBs(fallthroughFrom, fallthroughTo, path)) {
      totalCounters += (path.size() + 1) * fc.second;
      // Note, fallthroughFrom/to are not included in "path".
      hotSymbols.insert(fallthroughFrom);
      hotSymbols.insert(fallthroughTo);
      for (auto *sym : path) {
        if (sym->isLandingPadBlock()) {
          LOG(WARNING) << "*** HOT LANDING PAD: " << sym->fname.str() << "\t"
                       << sym->containingFunc->fname.str() << "\n";
        }
        extraBBsIncludedInFallthroughs += hotSymbols.insert(sym).second ? 1 : 0;
      }

      path.push_back(fallthroughTo);
      auto * fromSym = fallthroughFrom;
      for(auto * sym: path) {
        CFGNode *fromN = symbolNodeMap[fromSym];
        CFGNode *toN = symbolNodeMap[sym];
        if (fromN && toN && fromN != toN)
          fromN->controlFlowGraph->createEdge(fromN, toN, llvm::propeller::CFGEdge::INTRA_FUNC)->weight += fc.second;
        fromSym = sym;
      }
    }
  }
  this->fallthroughsWritten = fallthroughCountersBySymbol.size();
}

template <class ELFT>
bool fillELFPhdr(llvm::object::ELFObjectFileBase *ebFile,
                 map<uint64_t, ProgSegLoad> &phdrLoadMap) {
  ELFObjectFile<ELFT> *eobj = dyn_cast<ELFObjectFile<ELFT>>(ebFile);
  if (!eobj) return false;
  const ELFFile<ELFT> *efile = eobj->getELFFile();
  if (!efile) return false;
  auto program_headers = efile->program_headers();
  if (!program_headers) return false;
  for (const typename ELFT::Phdr &phdr : *program_headers) {
    if (phdr.p_type != llvm::ELF::PT_LOAD ||
        ((phdr.p_flags & llvm::ELF::PF_X) == 0))
      continue;
    if (phdr.p_paddr != phdr.p_vaddr) {
      LOG(ERROR) << "ELF type not supported: segment load vaddr != paddr";
      return false;
    }

    auto e = phdrLoadMap.find(phdr.p_offset);
    if (e == phdrLoadMap.end()) {
      phdrLoadMap[phdr.p_offset] = ProgSegLoad{.offset = phdr.p_offset,
                                               .vaddr = phdr.p_vaddr,
                                               .filesz = phdr.p_filesz,
                                               .memsz = phdr.p_memsz};
    } else {
      LOG(ERROR) << "Duplicate phdr load segment found in elf binary file.";
      return false;
    }
  }
  if (phdrLoadMap.empty()) {
    LOG(ERROR) << "No loadable and executable segments found in binary.";
    return false;
  }
  stringstream ss;
  ss << "Loadable and executable segments:\n";
  for (auto &seg : phdrLoadMap) {
    ss << "\toffset=" << hex0x << seg.first << ", vaddr=" << hex0x
       << seg.second.vaddr << ", filesz=" << hex0x << seg.second.filesz
       << std::endl;
  }
  LOG(INFO) << ss.str();
  return true;
}

bool PropellerProfWriter::initBinaryFile() {
  auto fileOrError = llvm::MemoryBuffer::getFile(binaryFileName);
  if (!fileOrError) {
    LOG(ERROR) << "Failed to read file '" << binaryFileName << "'.";
    return false;
  }
  this->binaryFileContent = std::move(*fileOrError);

  auto objOrError = llvm::object::ObjectFile::createELFObjectFile(
      llvm::MemoryBufferRef(*(this->binaryFileContent)));
  if (!objOrError) {
    LOG(ERROR) << "Not a valid ELF file '" << binaryFileName << "'.";
    return false;
  }
  this->objFile = std::move(*objOrError);

  auto *elfObjBase = dyn_cast<ELFObjectFileBase, ObjectFile>(objFile.get());
  binaryIsPIE = (elfObjBase->getEType() == llvm::ELF::ET_DYN);
  if (binaryIsPIE) {
    const char *elfIdent = binaryFileContent->getBufferStart();
    const char elfClass = elfIdent[4];
    const char elfData = elfIdent[5];
    if (elfClass == 1 && elfData == 1) {
      fillELFPhdr<llvm::object::ELF32LE>(elfObjBase, phdrLoadMap);
    } else if (elfClass == 1 && elfData == 2) {
      fillELFPhdr<llvm::object::ELF32BE>(elfObjBase, phdrLoadMap);
    } else if (elfClass == 2 && elfData == 1) {
      fillELFPhdr<llvm::object::ELF64LE>(elfObjBase, phdrLoadMap);
    } else if (elfClass == 2 && elfData == 2) {
      fillELFPhdr<llvm::object::ELF64BE>(elfObjBase, phdrLoadMap);
    } else {
      assert(false);
    }
  }
  LOG(INFO) << "'" << this->binaryFileName
            << "' is PIE binary: " << binaryIsPIE;
  return true;
}

bool PropellerProfWriter::parsePerfData() {
  this->perfDataFileParsed = 0;
  StringRef fn(perfFileName);
  while (!fn.empty()) {
    StringRef perfName;
    std::tie(perfName, fn) = fn.split(',');
    if (!parsePerfData(perfName.str())) {
      return false;
    }
    ++perfDataFileParsed;
  }
  LOG(INFO) << "Processed " << perfDataFileParsed << " perf file(s).";
  return true;
}

bool PropellerProfWriter::parsePerfData(const string &pName) {
  quipper::PerfReader perfReader;
  if (!perfReader.ReadFile(pName)) {
    LOG(ERROR) << "Failed to read perf data file: " << pName;
    return false;
  }

  quipper::PerfParser parser(&perfReader);
  if (!parser.ParseRawEvents()) {
    LOG(ERROR) << "Failed to parse perf raw events for perf file: '" << pName
               << "'.";
    return false;
  }

  if (!FLAGS_ignore_build_id) {
    if (!setupBinaryMMapName(perfReader, pName)) {
      return false;
    }
  }

  if (!setupMMaps(parser, pName)) {
    LOG(ERROR) << "Failed to find perf mmaps for binary '" << binaryFileName
               << "'.";
    return false;
  }

  return aggregateLBR(parser);
}

bool PropellerProfWriter::setupMMaps(quipper::PerfParser &parser,
                                     const string &pName) {
  // Depends on the binary file name, if
  //   - it is absolute, compares it agains the full path
  //   - it is relative, only compares the file name part
  // Note: compFunc is constructed in a way so that there is no branch /
  // conditional test inside the function.
  struct BinaryNameComparator {
    BinaryNameComparator(const string &binaryFileName) {
      if (llvm::sys::path::is_absolute(binaryFileName)) {
        comparePart = StringRef(binaryFileName);
        pathChanger = NullPathChanger;
      } else {
        comparePart = llvm::sys::path::filename(binaryFileName);
        pathChanger = NameOnlyPathChanger;
      }
    }

    bool operator()(const string &path) {
      return comparePart == pathChanger(path);
    }

    StringRef comparePart;
    std::function<StringRef(const string &)> pathChanger;

    static StringRef NullPathChanger(const string &sym) {
      return StringRef(sym);
    }
    static StringRef NameOnlyPathChanger(const string &sym) {
      return llvm::sys::path::filename(StringRef(sym));
    }
  } compFunc(FLAGS_match_mmap_file.empty()
                 ? (this->binaryMMapName.empty() ? binaryFileName
                                                 : this->binaryMMapName)
                 : FLAGS_match_mmap_file);

  for (const auto &pe : parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *eventPtr = pe.event_ptr;
    if (eventPtr->event_type_case() !=
        quipper::PerfDataProto_PerfEvent::kMmapEvent)
      continue;

    const quipper::PerfDataProto_MMapEvent &mmap = eventPtr->mmap_event();
    if (!mmap.has_filename()) continue;

    const string &MMapFileName = mmap.filename();
    if (!compFunc(MMapFileName) || !mmap.has_start() || !mmap.has_len() ||
        !mmap.has_pid())
      continue;

    if (this->binaryMMapName.empty()) {
      this->binaryMMapName = MMapFileName;
    } else if (binaryMMapName != MMapFileName) {
      LOG(ERROR) << "'" << binaryFileName
                 << "' is not specific enough. It matches both '"
                 << binaryMMapName << "' and '" << MMapFileName
                 << "' in the perf data file '" << pName
                 << "'. Consider using absolute file name.";
      return false;
    }
    uint64_t loadAddr = mmap.start();
    uint64_t loadSize = mmap.len();
    uint64_t pageOffset = mmap.has_pgoff() ? mmap.pgoff() : 0;

    // For the same binary, mmap can only be different if it is a PIE binary. So
    // for non-PIE binaries, we check all MMaps are equal and merge them into
    // binaryMMapByPid[0].
    uint64_t mPid = binaryIsPIE ? mmap.pid() : 0;
    set<MMapEntry> &loadMap = binaryMMapByPid[mPid];
    // Check for mmap conflicts.
    if (!checkBinaryMMapConflictionAndEmplace(loadAddr, loadSize, pageOffset,
                                              loadMap)) {
      stringstream ss;
      ss << "Found conflict mmap event: "
         << MMapEntry{loadAddr, loadSize, pageOffset}
         << ". Existing mmap entries: " << std::endl;
      for (auto &me : loadMap) {
        ss << "\t" << me << std::endl;
      }
      LOG(ERROR) << ss.str();
      return false;
    }
  }  // End of iterating mmap events.

  if (!std::accumulate(
          binaryMMapByPid.begin(), binaryMMapByPid.end(), 0,
          [](uint64_t V, const decltype(binaryMMapByPid)::value_type &sym)
              -> uint64_t { return V + sym.second.size(); })) {
    LOG(ERROR) << "Failed to find mmap entries in '" << pName << "' for '"
               << binaryFileName << "'.";
    return false;
  }
  for (auto &mpid : binaryMMapByPid) {
    stringstream ss;
    ss << "Found mmap in '" << pName << "' for binary: '" << binaryFileName
       << "', pid=" << ::dec << mpid.first << " (0 for non-pie executables)"
       << std::endl;
    for (auto &mm : mpid.second) {
      ss << "\t" << mm << std::endl;
    }
    LOG(INFO) << ss.str();
  }
  return true;
}

bool PropellerProfWriter::checkBinaryMMapConflictionAndEmplace(
    uint64_t loadAddr, uint64_t loadSize, uint64_t pageOffset,
    set<MMapEntry> &mm) {
  for (const MMapEntry &e : mm) {
    if (e.loadAddr == loadAddr && e.loadSize == loadSize &&
        e.pageOffset == pageOffset)
      return true;
    if (!((loadAddr + loadSize <= e.loadAddr) ||
          (e.loadAddr + e.loadSize <= loadAddr)))
      return false;
  }
  auto r = mm.emplace(loadAddr, loadSize, pageOffset);
  assert(r.second);
  return true;
}

bool PropellerProfWriter::setupBinaryMMapName(quipper::PerfReader &perfReader,
                                              const string &pName) {
  this->binaryMMapName = "";
  if (FLAGS_ignore_build_id || this->binaryBuildId.empty()) {
    return true;
  }
  list<pair<string, string>> existingBuildIds;
  for (const auto &buildId : perfReader.build_ids()) {
    if (buildId.has_filename() && buildId.has_build_id_hash()) {
      string PerfBuildId = buildId.build_id_hash();
      quipper::PerfizeBuildIDString(&PerfBuildId);
      existingBuildIds.emplace_back(buildId.filename(), PerfBuildId);
      if (PerfBuildId == this->binaryBuildId) {
        this->binaryMMapName = buildId.filename();
        LOG(INFO) << "Found file with matching buildId in perf file '" << pName
                  << "': " << this->binaryMMapName;
        return true;
      }
    }
  }
  stringstream ss;
  ss << "No file with matching buildId in perf data '" << pName
     << "', which contains the following <file, buildid>:" << std::endl;
  for (auto &p : existingBuildIds) {
    ss << "\t" << p.first << ": " << BuildIdWrapper(p.second.c_str())
       << std::endl;
  }
  LOG(INFO) << ss.str();
  return false;
}

bool PropellerProfWriter::aggregateLBR(quipper::PerfParser &parser) {
  uint64_t brStackCount = 0;
  for (const auto &pe : parser.parsed_events()) {
    quipper::PerfDataProto_PerfEvent *eventPtr = pe.event_ptr;
    if (eventPtr->event_type_case() !=
        quipper::PerfDataProto_PerfEvent::kSampleEvent)
      continue;

    auto &sEvent = eventPtr->sample_event();
    if (!sEvent.has_pid()) continue;
    auto brStack = sEvent.branch_stack();
    if (brStack.empty()) continue;
    uint64_t pid = binaryIsPIE ? sEvent.pid() : 0;
    if (binaryMMapByPid.find(pid) == binaryMMapByPid.end()) continue;
    auto &branchCounters = branchCountersByPid[pid];
    auto &fallthroughCounters = fallthroughCountersByPid[pid];
    uint64_t lastFrom = INVALID_ADDRESS;
    uint64_t lastTo = INVALID_ADDRESS;
    brStackCount += brStack.size();
    std::vector<SymbolEntry *> symSeq{};
    SymbolEntry *LastToSym = nullptr;
    for (int p = brStack.size() - 1; p >= 0; --p) {
      const auto &be = brStack.Get(p);
      uint64_t from = be.from_ip();
      uint64_t to = be.to_ip();
      // TODO: LBR sometimes duplicates the first entry by mistake. For now we
      // treat these to be true entries.

      // if (p == 0 && from == lastFrom && to == lastTo) {
      //  LOG(INFO) << "Ignoring duplicate LBR entry: 0x" << std::hex <<
      //             from
      //             << "-> 0x" << to << std::dec << "\n";
      //  continue;
      //}

      ++(branchCounters[make_pair(from, to)]);
      if (lastTo != INVALID_ADDRESS && lastTo <= from)
        ++(fallthroughCounters[make_pair(lastTo, from)]);

      // Aggregate path profile information.
      lastTo = to;
      lastFrom = from;
    }  // end of iterating one br record
  }  // End of iterating all br records.
  if (brStackCount < 100) {
    LOG(ERROR) << "Too few brstack records (only " << brStackCount
               << " record(s) found), cannot continue.";
    return false;
  }
  LOG(INFO) << "Processed " << CommaF(brStackCount) << " lbr records.";
  return true;
}

bool PropellerProfWriter::findBinaryBuildId() {
  this->binaryBuildId = "";
  if (FLAGS_ignore_build_id) return true;
  bool buildIdFound = false;
  for (auto &sr : objFile->sections()) {
    llvm::object::ELFSectionRef esr(sr);
    StringRef sName;
    auto exSecRefName = sr.getName();
    if (exSecRefName) {
      sName = *exSecRefName;
    } else {
      continue;
    }
    auto expectedSContents = sr.getContents();
    if (expectedSContents && esr.getType() == llvm::ELF::SHT_NOTE && sName == ".note.gnu.build-id"
        && !expectedSContents->empty()) {
      StringRef sContents = *expectedSContents;
      const unsigned char *p = sContents.bytes_begin() + 0x10;
      if (p >= sContents.bytes_end()) {
        LOG(INFO) << "Section '.note.gnu.build-id' does not contain valid "
                     "build id information.";
        return true;
      }
      string buildId((const char *)p, sContents.size() - 0x10);
      quipper::PerfizeBuildIDString(&buildId);
      this->binaryBuildId = buildId;
      LOG(INFO) << "Found Build Id in binary '" << binaryFileName
                << "': " << BuildIdWrapper(buildId.c_str());
      return true;
    }
  }
  LOG(INFO) << "No Build Id found in '" << binaryFileName << "'.";
  return true;  // always returns true
}

#endif
