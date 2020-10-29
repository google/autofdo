#ifndef AUTOFDO_LLVM_PROPELLER_STATISTICS_H_
#define AUTOFDO_LLVM_PROPELLER_STATISTICS_H_

#include <cstdint>

namespace devtools_crosstool_autofdo {
struct PropellerStats {
  int binary_mmap_num = 0;
  uint64_t br_counters_accumulated = 0;
  uint64_t edges_with_same_src_sink_but_different_type = 0;
  uint64_t dropped_symbols = 0;
  uint64_t syms_created = 0;
  uint64_t cfgs_created = 0;
  uint64_t nodes_created = 0;
  uint64_t edges_created = 0;
  uint64_t duplicate_symbols = 0;
  uint64_t bbaddrmap_function_does_not_have_symtab_entry = 0;

  // Merge two copies of stats.
  PropellerStats & operator += (const PropellerStats &s) {
    binary_mmap_num += s.binary_mmap_num;
    br_counters_accumulated += s.br_counters_accumulated;
    edges_with_same_src_sink_but_different_type +=
        s.edges_with_same_src_sink_but_different_type;
    dropped_symbols += s.dropped_symbols;
    syms_created += s.syms_created;
    cfgs_created += s.cfgs_created;
    nodes_created += s.nodes_created;
    edges_created += s.edges_created;
    duplicate_symbols += s.duplicate_symbols;
    bbaddrmap_function_does_not_have_symtab_entry +=
        s.bbaddrmap_function_does_not_have_symtab_entry;
    return *this;
  }
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_STATISTICS_H_
