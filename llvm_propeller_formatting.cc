#include "llvm_propeller_formatting.h"

#include <iomanip>
#include <ios>
#include <list>
#include <ostream>
#include <string>

#include "llvm_propeller_cfg.h"

namespace devtools_crosstool_autofdo {

std::ostream &operator<<(std::ostream &out, const CFGEdgeNameFormatter &f) {
  if (f.edge == nullptr) return out << "nullptr-edge";
  return out << f.edge->src()->GetName() << " -> " << f.edge->sink()->GetName()
             << "[ weight: " << f.edge->weight()
             << "] [type: " << CFGEdge::GetCfgEdgeKindString(f.edge->kind())
             << "]";
}

std::ostream &operator<<(std::ostream &out, const AddressFormatter &f) {
  auto flags = out.flags();
  out << std::showbase << std::hex << f.addr << std::noshowbase << std::dec;
  out.flags(flags);
  return out;
}

}  // namespace devtools_crosstool_autofdo
