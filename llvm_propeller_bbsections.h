#ifndef AUTOFDO_LLVM_PROPELLER_BBSECTIONS_H_
#define AUTOFDO_LLVM_PROPELLER_BBSECTIONS_H_

#include "base/logging.h"  // For "CHECK".
#if defined(HAVE_LLVM)
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {
// This data structure wraps all the information for basic block symbols.
struct SymbolEntry {
  using AliasesTy = llvm::SmallVector<llvm::StringRef, 3>;

  SymbolEntry(uint64_t o, llvm::StringRef n, AliasesTy as, uint64_t address,
              uint64_t s, SymbolEntry *parent_func, uint32_t md)
      : ordinal(o),
        name(n),
        aliases(std::move(as)),
        addr(address),
        size(s),
        metadata(md),
        hot_tag(false),
        func_ptr(parent_func ? parent_func : this) {}

  // Unique index number across all symbols that participate linking.
  uint64_t ordinal;
  // For a function symbol, it's the symbol name.
  // For a bb symbol this is always "".
  llvm::StringRef name;
  // Only valid for function symbols. And aliases[0] always equals to name.
  // For example, SymbolEntry.name = "foo", SymbolEntry.aliases
  // = {"foo", "foo2", "foo3"}.
  AliasesTy aliases;
  uint64_t addr;
  uint64_t size;
  const uint32_t metadata;
  bool hot_tag = false;
  // For basic block symbols, this is the containing function pointer, for a
  // function symbol, this points to itself. This is never nullptr.
  SymbolEntry * func_ptr;

  bool IsFunction() const { return func_ptr == this; }
  bool IsBasicBlock() const { return func_ptr != this; }

  // Metadata accessors.
  bool IsReturnBlock() const { return metadata & kMetaReturnBlockMask; }
  bool IsTailCallBlock() const { return metadata & kMetaTailCallMask; }
  bool IsEhPadBlock() const { return metadata & kMetaEhPadMask; }
  // Whether this block can fall through to its next.
  bool CanFallThrough() const { return metadata & kMetaFallThroughMask; }

  bool operator<(const SymbolEntry &other) const {
    return ordinal < other.ordinal;
  }

  bool operator==(const SymbolEntry &other) const {
    return ordinal == other.ordinal;
  }

  bool operator!=(const SymbolEntry &other) const {
    return ordinal != other.ordinal;
  }

  static constexpr uint64_t kInvalidAddress = static_cast<uint64_t>(-1);
  static constexpr uint64_t kMetaReturnBlockMask = 1;
  static constexpr uint64_t kMetaTailCallMask = (1 << 1);
  static constexpr uint64_t kMetaEhPadMask = (1 << 2);
  static constexpr uint64_t kMetaFallThroughMask = (1 << 3);
};

// We sometime use "SymbolEntry *" as set element and map key, we need to use
// a stable comparator for "SymbolEntry *", otherwise to depend on address
// comparison is unstable.
struct SymbolPtrComparator {
  bool operator()(const SymbolEntry *s1, const SymbolEntry *s2) const {
    DCHECK(s1 && s2);
    if (s1 == s2) return false;
    return *s1 < *s2;
  }
};

struct SymbolUniquePtrComparator {
  bool operator()(const std::unique_ptr<SymbolEntry> &s1,
                  const std::unique_ptr<SymbolEntry> &s2) const {
    DCHECK(s1 && s2);
    if (*s1 == *s2) return false;
    return *s1 < *s2;
  }
};

using SymbolPtrPair = std::pair<const SymbolEntry *, const SymbolEntry *>;
struct SymbolPtrPairComparator {
  bool operator()(const SymbolPtrPair &p1, const SymbolPtrPair &p2) const {
    if (p1.first != p2.first) return SymbolPtrComparator()(p1.first, p2.first);
    if (p1.second != p2.second)
      return SymbolPtrComparator()(p1.second, p2.second);
    return false;
  }
};

}  // namespace devtools_crosstool_autofdo

#endif
#endif  // AUTOFDO_LLVM_PROPELLER_BBSECTIONS_H_
