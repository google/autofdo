#include "config.h"
#if defined(HAVE_LLVM)
#include <vector>

#include "llvm_propeller_path_profile.h"
#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_profile_writer.h"
#include "llvm_propeller_profile_format.h"

#include <iostream>
#include <unordered_map>

bool PathProfile::addSymSeq(std::vector<SymbolEntry *> &symSeq) {
  return false;
}

bool Path::tryMerge(const Path &path, Path &mergedPath) const {
  return true;
}

bool Path::tryCollapseLoop() {
  return true;
}

void PathProfile::printPaths(std::ostream &out, PropellerProfWriter &ppWriter) {
}

bool Path::expandToIncludeFallthroughs(PropellerProfWriter &ppWriter) {
  return true;
}

#endif
