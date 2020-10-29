#ifndef AUTOFDO_LLVM_PROPELLER_FORMATTING_H_  // NOLINT(build/header_guard)
#define AUTOFDO_LLVM_PROPELLER_FORMATTING_H_

#include <cstdint>  // for uint64_t
#include <ostream>

namespace devtools_crosstool_autofdo {

class CFGEdge;
struct SymbolEntry;

struct CommaStyleNumberFormatter {
  explicit CommaStyleNumberFormatter(uint64_t v) : value(v) {}
  uint64_t value;
};

struct SymbolNameFormatter {
  explicit SymbolNameFormatter(const SymbolEntry &s) : sym(&s) {}
  explicit SymbolNameFormatter(const SymbolEntry *s) : sym(s) {}

  const SymbolEntry *sym;
};

struct CFGEdgeNameFormatter {
  explicit CFGEdgeNameFormatter(const CFGEdge *e) : edge(e) {}
  explicit CFGEdgeNameFormatter(const CFGEdge &e) : edge(&e){}

  const CFGEdge *edge;
};

struct AddressFormatter {
  explicit AddressFormatter(uint64_t a): addr(a) {}

  uint64_t addr;
};

std::ostream &operator<<(std::ostream &out, const CommaStyleNumberFormatter &f);

std::ostream &operator<<(std::ostream &out, const SymbolNameFormatter &f);

std::ostream &operator<<(std::ostream &out, const CFGEdgeNameFormatter &f);

std::ostream &operator<<(std::ostream &out, const AddressFormatter &f);

}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_FORMATTING_H_
