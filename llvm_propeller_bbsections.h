#ifndef LLVM_PROFILEDATA_PROPELLERPROF_H
#define LLVM_PROFILEDATA_PROPELLERPROF_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

using llvm::StringRef;

namespace llvm {
namespace propeller {

struct SymbolEntry {
  using AliasesTy = SmallVector<StringRef, 3>;

  SymbolEntry(uint64_t o, const StringRef &n, AliasesTy &&as, uint32_t bdx,
              uint64_t address, uint64_t s, SymbolEntry *funcptr, uint32_t md)
      : ordinal(o), fname(n), aliases(std::move(as)), bbindex(bdx),
        addr(address), size(s), hotTag(false),
        containingFunc(funcptr ? funcptr : this), metadata(md) {}

  // Unique index number across all symbols that participate linking.
  const uint64_t ordinal;
  // Function name, only valid when this is a function symbol. For bb symbols, this equals to "".
  const StringRef fname;
  // Only valid when this is a function symbol.
  const AliasesTy aliases;
  // Basic block index, starts from 0. For functin symbols, this is kInvalidBasicBlockIndex.
  const uint32_t bbindex;
  const uint64_t addr;
  const uint64_t size;

  bool hotTag; // Whether this symbol is listed in the propeller section.
  // For bbTag symbols, this is the containing fuction pointer, for a normal
  // function symbol, this points to itself. This is neverl nullptr.
  SymbolEntry *containingFunc;

  const uint32_t metadata;

  bool canFallthrough() const { return true; }

  bool isReturnBlock() const { return kMetaReturnBlockMask & metadata; }

  bool isLandingPadBlock() const { return kMetaEhPadMask & metadata; }

  bool operator<(const SymbolEntry &Other) const {
    return ordinal < Other.ordinal;
  }

  bool isFunction() const { return containingFunc == this || !containingFunc; }
  bool isBasicBlock() const { return containingFunc && containingFunc != this; }

  struct OrdinalLessComparator {
    bool operator()(const SymbolEntry *s1, const SymbolEntry *s2) const {
      if (s1 && s2)
        return s1->ordinal < s2->ordinal;
      return !!s1 < !!s2;
    }
  };

  static constexpr uint32_t kInvalidBasicBlockIndex = static_cast<uint32_t>(-1);
  static constexpr uint64_t kInvalidAddress = static_cast<uint64_t>(-1);
  static constexpr uint64_t kMetaReturnBlockMask = 1;
  static constexpr uint64_t kMetaTailCallMask = (1 << 1);
  static constexpr uint64_t kMetaEhPadMask = (1 << 2);
};

} // namespace propeller
} // namespace llvm

#endif
