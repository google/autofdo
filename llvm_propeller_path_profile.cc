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
      // Duplicated Symbols appears in the path and they belong to different
      // branches, so return false; 
      // mnp[csym] = i;  // update the map
      return false;
    }
  } while (i != syms.end());
  return true;
}

void PathProfile::printPaths(std::ostream &out, PropellerProfWriter &ppWriter) {
  for (auto &fp : funcPaths) {
    int t = 0;
    Path *heaviestPath = nullptr;
    for (auto &p : fp.second) {
      if (p.weight > 1000) {
        if (!p.tryCollapseLoop()) continue;
        p.expandToIncludeFallthroughs(ppWriter);
        if (!heaviestPath || heaviestPath->density() < p.density())
          heaviestPath = &p;
        // const uint64_t adjust = p.weight / p.length();
        // for (int i = 1; adjust > 10 && i < p.length(); ++i) {
        //   SymbolEntry *from = p.syms[i-1];
        //   SymbolEntry *to = p.syms[i];
        //   out << SymOrdinalF(from) << " " << SymOrdinalF(to) << " "
        //       << countF(adjust) << " P" << std::endl;
        // }

        // auto lengthBeforeCollapse = p.length();
        // out << ">>>>>>\n";
        // out << "      Prestine path " << ++t << ": " << p << std::endl;
        // p.tryCollapseLoop();
        // if (lengthBeforeCollapse != p.length())
        //   out << "    Collapsed into: " << p << std::endl;
        // auto lengthBeforeExpand = p.length();
        // p.expandToIncludeFallthroughs(ppWriter);
        // if (lengthBeforeExpand != p.length())
        //   out << "    Expanded into: " << p << std::endl;
        // out << "<<<<<<\n";
      }
    }
    if (heaviestPath) {
      out << heaviestPath->getFuncName().str();
      for (auto *sym: heaviestPath->syms) 
        out << " " << SymOrdinalF(sym);
      out << " P\n";
    }
  }
}

bool Path::expandToIncludeFallthroughs(PropellerProfWriter &ppWriter) {
  // Use list so iterators do not get invalidated after insertion.
  std::list<SymbolEntry *> symList(syms.begin(), syms.end());
  std::list<uint64_t> cntList(cnts.begin(), cnts.end());
  bool changed = false;
  auto from = symList.begin(), e = symList.end();
  auto to = std::next(from);
  auto wFrom = cntList.begin();
  auto wTo = std::next(wFrom);
  for (; to != e; ++to, ++wTo) {
    vector<SymbolEntry *> fts;
    if ((*from)->addr >= (*to)->addr ||
        !ppWriter.calculateFallthroughBBs(*from, *to, fts))
      continue;
    if (!fts.empty()) {
      // Note, after this operation, no iterators are invalidated.
      symList.insert(to, fts.begin(), fts.end());
      cntList.insert(wTo, fts.size(), ((*wFrom + *wTo) >> 1));
      weight += fts.size() * ((*wFrom + *wTo) >> 1);
      changed = true;
    }
    from = to;
    wFrom = wTo;
  }
  if (changed) {
    syms = std::vector<SymbolEntry *>(symList.begin(), symList.end());
    cnts = std::vector<uint64_t>(cntList.begin(), cntList.end());
  }
  return true;
}

#endif
