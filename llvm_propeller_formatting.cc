#include "llvm_propeller_formatting.h"

#include <iomanip>
#include <ios>
#include <iostream>
#include <list>

#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"

namespace devtools_crosstool_autofdo {

// Output number with "," separated style.
std::ostream &operator<<(std::ostream &out,
                         const CommaStyleNumberFormatter &f) {
  std::list<int> seg;
  uint64_t value = f.value;
  while (value) {
    seg.insert(seg.begin(), value % 1000);
    value /= 1000;
  }
  if (seg.empty()) seg.insert(seg.begin(), 0);
  auto of = out.fill();
  auto ow = out.width();
  auto i = seg.begin();
  out << std::dec << std::noshowbase << std::setfill('\0') << *i;
  for (++i; i != seg.end(); ++i)
    out << "," << std::setw(3) << std::setfill('0') << *i;
  out.fill(of);
  out.width(ow);
  return out;
}

static std::ostream &OutputAliases(std::ostream &out, const SymbolEntry &s) {
  int k = 0;
  for (const auto &n : s.aliases) {
    if (k++) out << '/';
    out << n.str();
  }
  return out;
}

std::ostream &operator<<(std::ostream &out, const SymbolNameFormatter &f) {
  if (f.sym == nullptr) return out << "nullptr-symbol";
  if (f.sym->IsBasicBlock())
    out << f.sym->ordinal - f.sym->func_ptr->ordinal << ".BB.";
  return OutputAliases(out, *(f.sym->func_ptr));
}

std::ostream &operator<<(std::ostream &out, const CFGEdgeNameFormatter &f) {
  if (f.edge == nullptr) return out << "nullptr-edge";
  return out << f.edge->src_->GetName() << " -> " << f.edge->sink_->GetName()
             << "[ weight: " << f.edge->weight_ << "]";
}

std::ostream &operator<<(std::ostream &out, const AddressFormatter &f) {
  auto flags = out.flags();
  out << std::showbase << std::hex << f.addr << std::noshowbase
             << std::dec;
  out.flags(flags);
  return out;
}

}  // namespace devtools_crosstool_autofdo
