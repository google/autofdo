#include "llvm_propeller_formatting.h"

#include <iomanip>
#include <ios>
#include <list>
#include <ostream>
#include <string>

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

std::ostream &operator<<(std::ostream &out, const CFGEdgeNameFormatter &f) {
  if (f.edge == nullptr) return out << "nullptr-edge";
  return out << f.edge->src()->GetName() << " -> " << f.edge->sink()->GetName()
             << "[ weight: " << f.edge->weight()
             << "] [type: " << CFGEdge::GetCfgEdgeKindString(f.edge->kind())
             << "]";
}

std::ostream &operator<<(std::ostream &out, const AddressFormatter &f) {
  auto flags = out.flags();
  out << std::showbase << std::hex << f.addr << std::noshowbase
             << std::dec;
  out.flags(flags);
  return out;
}

}  // namespace devtools_crosstool_autofdo
