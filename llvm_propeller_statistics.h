#ifndef AUTOFDO_LLVM_PROPELLER_STATISTICS_H_
#define AUTOFDO_LLVM_PROPELLER_STATISTICS_H_

#include <cstdint>

namespace devtools_crosstool_autofdo {
struct PropellerStats {
  int binary_mmap_num = 0;
  int perf_file_parsed = 0;
  uint64_t br_counters_accumulated = 0;
  uint64_t edges_with_same_src_sink_but_different_type = 0;
  uint64_t cfgs_created = 0;
  uint64_t nodes_created = 0;
  uint64_t edges_created = 0;
  uint64_t duplicate_symbols = 0;
  uint64_t bbaddrmap_function_does_not_have_symtab_entry = 0;
  uint64_t original_intra_score = 0;
  uint64_t optimized_intra_score = 0;
  uint64_t original_inter_score = 0;
  uint64_t optimized_inter_score = 0;
  uint64_t hot_functions = 0;

  // Merge two copies of stats.
  PropellerStats & operator += (const PropellerStats &s) {
    binary_mmap_num += s.binary_mmap_num;
    perf_file_parsed += s.perf_file_parsed;
    br_counters_accumulated += s.br_counters_accumulated;
    edges_with_same_src_sink_but_different_type +=
        s.edges_with_same_src_sink_but_different_type;
    cfgs_created += s.cfgs_created;
    nodes_created += s.nodes_created;
    edges_created += s.edges_created;
    duplicate_symbols += s.duplicate_symbols;
    bbaddrmap_function_does_not_have_symtab_entry +=
        s.bbaddrmap_function_does_not_have_symtab_entry;
    original_intra_score += s.original_intra_score;
    optimized_intra_score += s.optimized_intra_score;
    original_inter_score += s.original_inter_score;
    optimized_inter_score += s.optimized_inter_score;
    hot_functions += s.hot_functions;
    return *this;
  }
};
}  // namespace devtools_crosstool_autofdo
#endif  // AUTOFDO_LLVM_PROPELLER_STATISTICS_H_
