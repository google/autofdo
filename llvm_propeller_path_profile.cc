#include "config.h"
#if defined(HAVE_LLVM)
#include <vector>

#include "llvm/ProfileData/BBSectionsProf.h"
#include "llvm_propeller_path_profile.h"
#include "llvm_propeller_profile_format.h"

#include <iostream>

bool PathProfile::addSymSeq(std::vector<SymbolEntry *> &symSeq) {
  if (symSeq.size() < MIN_LENGTH) return false;

  SymbolEntry *func = symSeq[0]->containingFunc;
  assert(func);
  auto fi = funcPaths.find(func->name);
  if (fi == funcPaths.end()) {
    funcPaths[func->name].emplace(std::move(symSeq));
    return false;
  }

  Path p1(std::move(symSeq));
  const Path::Key k1 = p1.pathKey();
  auto pi = fi->second.begin(), pe = fi->second.end();
  Path merged;
  for (; pi != pe; ++pi) {
    const Path &p0 = *pi;
    if (p0.tryMerge(p1, merged)) {
      fi->second.erase(pi);
      fi->second.emplace(std::move(merged));
      return true;
    }
  }
  fi->second.emplace(std::move(p1));
  return false;
}

bool Path::tryMerge(const Path &path, Path &mergedPath) const {
  mergedPath.syms.clear();
  mergedPath.cnts.clear();
  mergedPath.weight = 0;
  Key k1 = pathKey();
  Key k2 = path.pathKey();
  Path const *main;
  Path const *sub;
  if (syms.size() > path.syms.size()) {
    main = this;
    sub = &path;
  } else {
    main = &path;
    sub = this;
  }

  // main now points to the longer chain.

  std::vector<SymbolEntry *> newSyms;
  std::vector<uint64_t> newCnts;
  uint64_t newWeight;
  uint64_t subHeadAddr = sub->syms[0]->addr;
  auto symi = main->syms.begin(), symend = main->syms.end(),
       subsymi = sub->syms.begin(), subsymend = sub->syms.end();
  auto cnti = main->cnts.begin(), cntend = main->cnts.end(),
       subcnti = sub->cnts.begin(), subcntend = sub->cnts.end();
  assert(main->syms.size() == main->cnts.size());
  assert(sub->syms.size() == sub->cnts.size());
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
  copyRest(symi, main->syms.end(), cnti, main->cnts.end());
  copyRest(subsymi, sub->syms.end(), subcnti, main->cnts.end());
  newWeight = weight + path.weight;
  mergedPath = Path(std::move(newSyms), std::move(newCnts), newWeight);

  // std::cout << "(======\n";
  // this->print(std::cout);
  // path.print(std::cout);
  // std::cout << "\n======)==>";
  // mergedPath.print(std::cout);
  return true;
}

void PathProfile::printPaths(std::ostream &out) const {
  for (const auto &fp : funcPaths) {
      out << "Function paths: " << fp.first.str() << "\n";
      int t = 0;
      for (const auto &p : fp.second) {
        if (p.weight > 1000)
          out << "    Path " << ++t << ": " << p << std::endl;
      }
    }
}

#endif
