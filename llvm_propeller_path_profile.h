#if defined(HAVE_LLVM)
#ifndef _LLVM_PROPELLER_PATH_PROFILE_H_
#define _LLVM_PROPELLER_PATH_PROFILE_H_

#include "llvm/ADT/StringRef.h"
#include "llvm/ProfileData/BBSectionsProf.h"

#include <list>
#include <map>
#include <ostream>
#include <vector>

using llvm::propeller::SymbolEntry;
using llvm::StringRef;

class Path;

class PropellerProfWriter;
class Path {
 public:
  using Key = std::pair<uint64_t, uint64_t>;

  Path() {}

  explicit Path(Path &&path)
      : syms(std::move(path.syms)),
        cnts(std::move(path.cnts)),
        weight(path.weight) {
    assert(syms.size() == cnts.size());
  }

  explicit Path(std::vector<SymbolEntry *> &&o)
      : weight(o.size()), cnts(o.size(), 1), syms(std::move(o)) {
    assert(syms.size() == cnts.size());
  }

  explicit Path(std::vector<SymbolEntry *> &&o, std::vector<uint64_t> &&c,
                uint64_t w)
      : weight(w), cnts(std::move(c)), syms(std::move(o)) {
    assert(syms.size() == cnts.size());
  }

  const Path &operator=(Path &&o) {
    syms.clear();
    cnts.clear();
    syms = std::move(o.syms);
    cnts = std::move(o.cnts);
    weight = o.weight;
    return *this;
  }

  bool operator<(const Path &p2) const {
    if (syms.empty() || p2.syms.empty()) return syms.empty();
    return syms[0]->addr < p2.syms[0]->addr;
  }

  bool tryMerge(const Path &path, Path &mergedPath) const;

  bool tryCollapseLoop();

  bool expandToIncludeFallthroughs(PropellerProfWriter &ppWriter);

  const Key pathKey() const {
    return Key(syms[0]->addr, syms.back()->addr);
  }

  const StringRef getFuncName() const {
    return syms.empty() ? "" : syms[0]->containingFunc->name;
  }

  const uint64_t length() const { return syms.size(); }

  const std::ostream &print(std::ostream &out) const;

  // Data field.
  uint64_t weight;
  std::vector<uint64_t> cnts;
  std::vector<SymbolEntry *> syms;
};

class PathProfile {
 public:
  using FuncPathsTy = std::map<llvm::StringRef, std::list<Path>>;
  FuncPathsTy funcPaths;

  struct PathComparator {
    bool operator()(Path *p1, Path *p2) const {
      return p1->weight < p2->weight;
    }
  };

  bool addSymSeq(std::vector<SymbolEntry *> &symSequence);

  void printPaths(std::ostream &out, PropellerProfWriter &ppWriter);

  const static int MIN_LENGTH = 2;
};

#endif
#endif
