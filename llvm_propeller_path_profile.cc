#include "config.h"
#if defined(HAVE_LLVM)
#include <vector>

#include "llvm_propeller_profile_writer.h"
#include "llvm/ProfileData/BBSectionsProf.h"
#include "llvm_propeller_path_profile.h"
#include "llvm_propeller_profile_format.h"

#include <iostream>
#include <unordered_map>

bool PathProfile::addSymSeq(std::vector<SymbolEntry *> &symSeq) {
  if (symSeq.size() < MIN_LENGTH) return false;

  SymbolEntry *func = symSeq[0]->containingFunc;
  assert(func);
  auto fi = funcPaths.find(func->name);
  if (fi == funcPaths.end()) {
    funcPaths[func->name].emplace_back(std::move(symSeq));
    return false;
  }

  Path p1(std::move(symSeq));
  const Path::Key k1 = p1.pathKey();
  auto pi = fi->second.begin(), pe = fi->second.end();
  Path merged;
  for (; pi != pe; ++pi) {
    const Path &p0 = *pi;
    if (p0.tryMerge(p1, merged) || p1.tryMerge(p0, merged)) {
      fi->second.erase(pi);
      fi->second.emplace_back(std::move(merged));
      return true;
    }
  }
  fi->second.emplace_back(std::move(p1));
  return false;
}

bool Path::tryMerge(const Path &path, Path &mergedPath) const {
  mergedPath.syms.clear();
  mergedPath.cnts.clear();
  mergedPath.weight = 0;
  std::vector<SymbolEntry *> newSyms;
  std::vector<uint64_t> newCnts;
  uint64_t newWeight;
  uint64_t subHeadAddr = path.syms[0]->addr;
  auto symi = syms.begin(), symend = syms.end(),
       subsymi = path.syms.begin(), subsymend = path.syms.end();
  auto cnti = cnts.begin(), cntend = cnts.end(),
       subcnti = path.cnts.begin(), subcntend = path.cnts.end();
  assert(syms.size() == cnts.size());
  assert(path.syms.size() == path.cnts.size());
  bool headerFound = false;
  bool doMerge = false;
  for (; symi != symend && subsymi != subsymend; ++symi, ++cnti) {
    headerFound |= (subHeadAddr == (*symi)->addr);
    doMerge = ((*symi)->addr == (*subsymi)->addr);
    if (headerFound && !doMerge) break;
    if (headerFound && doMerge) {
      newSyms.push_back(*symi);
      newCnts.push_back(*cnti + (*subcnti));
      ++subcnti;
      ++subsymi;
    }
    if (!headerFound) {
      newSyms.push_back(*symi);
      newCnts.push_back(*cnti);
    }
  }
  if (!doMerge) return false;

  auto copyRest = [&newSyms, &newCnts](
                      std::vector<SymbolEntry *>::const_iterator i,
                      std::vector<SymbolEntry *>::const_iterator j,
                      std::vector<uint64_t>::const_iterator s,
                      std::vector<uint64_t>::const_iterator t) {
    for (; i != j; ++i, ++s) {
      newSyms.push_back(*i);
      newCnts.push_back(*s);
    }
  };
  copyRest(symi, syms.end(), cnti, cnts.end());
  copyRest(subsymi, path.syms.end(), subcnti, cnts.end());
  newWeight = weight + path.weight;
  mergedPath = Path(std::move(newSyms), std::move(newCnts), newWeight);

  // std::cout << "(======\n";
  // this->print(std::cout);
  // path.print(std::cout);
  // std::cout << "\n======)==>";
  // mergedPath.print(std::cout);
  return true;
}

bool Path::tryCollapseLoop() {
  using SymIter = std::vector<SymbolEntry *>::iterator;
  // This stores the last position that a symbol appears in the path.
  std::unordered_map<SymbolEntry *, SymIter> mnp;
  SymIter i = syms.begin();
  do {
    for (; i != syms.end(); ++i) {
      SymbolEntry *csym = *i;
      auto r = mnp.find(csym);
      if (r == mnp.end()) {
        mnp.emplace(csym, i);
        continue;
      }
      auto p = r->second;
      auto q = i;
      // *p == *i
      bool match = true;
      // Try to match [p, i) <==> [i, i + (i - p))
      for (; match && p != i && q != syms.end(); ++p, ++q)
        if (*p != *q) match = false;
      if (match && q != syms.end()) {
        p = r->second;   // reset p to start position.
        // Fold [p, i) <- [i, q) ==> [p, i)
        auto iCnt = cnts.begin() + (i - syms.begin());
        auto qCnt = cnts.begin() + (q - syms.begin());
        auto pCnt = cnts.begin() + (p - syms.begin());
        for (auto a = pCnt, b = iCnt; a != iCnt; ++a, ++b) *a += *b;
        // Deletion must happen at the end, because after erasion, all
        // iterators are invalidated, including end iterator.
        cnts.erase(iCnt, qCnt);
        // Now i points to the next element after collapsing.
        i = syms.erase(i, q);
        break;
      }
      mnp[csym] = i;  // update the map
    }
  } while (i != syms.end());
  return true;
}

void PathProfile::printPaths(std::ostream &out, PropellerProfWriter &ppWriter) {
  for (auto &fp : funcPaths) {
    out << "Function paths: " << fp.first.str() << "\n";
    int t = 0;
    for (auto &p : fp.second) {
      if (p.weight > 1000) {
        auto lengthBeforeCollapse = p.length();
        out << ">>>>>>\n";
        out << "      Prestine path " << ++t << ": " << p << std::endl;
        p.tryCollapseLoop();
        if (lengthBeforeCollapse != p.length())
          out << "    Collapsed into: " << p << std::endl;
        auto lengthBeforeExpand = p.length();
        p.expandToIncludeFallthroughs(ppWriter);
        if (lengthBeforeExpand != p.length())
          out << "    Expanded into: " << p << std::endl;
        out << "<<<<<<\n";
      }
    }
  }
}

bool Path::expandToIncludeFallthroughs(PropellerProfWriter &ppWriter) {
  auto from = syms.begin(), e = syms.end();
  auto to = std::next(from);
  auto wFrom = cnts.begin();
  auto wTo = std::next(wFrom);
  uint64_t lastCnt;
  SymbolEntry *lastTo = nullptr;
  for (; to != e; ++from, ++to, ++wFrom, ++wTo) {
    if (lastTo) {
      vector<SymbolEntry *> fts;
      if (!ppWriter.calculateFallthroughBBs(lastTo, (*from), fts))
        return false;
      if (!fts.empty()) {
        // Note, after this operation, from / to are still valid.
        syms.insert(from, fts.begin(), fts.end());
        cnts.insert(wFrom, fts.size(), ((*wFrom + lastCnt) >> 1));
      }
    }
    lastTo = *to;
    lastCnt = *wTo;
  }
  return true;
}

#endif
