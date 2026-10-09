// Source for testdata/dwarf5_line_tables_only.bin; see addr2line_test.cc.
static inline __attribute__((always_inline)) int mix(int x) {
  return (x << 3) ^ (x >> 2);
}

__attribute__((noinline)) int work(int n) {
  int s = 0;
  for (int i = 0; i < n % 17; ++i)
    s += mix(i + n);
  return s;
}
