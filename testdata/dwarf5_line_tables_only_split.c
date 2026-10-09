// Source for testdata/dwarf5_line_tables_only.bin; see addr2line_test.cc.
// split_cold is placed in its own section, so this unit is not contiguous and
// clang describes it with DW_AT_ranges (DW_FORM_rnglistx).
__attribute__((noinline, section(".text.split_cold"))) int split_cold(int n) {
  return n * 3 + 1;
}

__attribute__((noinline)) int split(int n) {
  return n > 100 ? split_cold(n) : n;
}
