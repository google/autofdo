// Source for testdata/dwarf5_line_tables_only.bin; see addr2line_test.cc.
#include <stdio.h>
#include <stdlib.h>

int work(int n);
int split(int n);

static inline __attribute__((always_inline)) int square_add(int a, int b) {
  return a * a + b;
}

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 1000;
  int acc = 0;
  for (int i = 0; i < n; ++i)
    acc = square_add(acc & 0xff, work(i));
  printf("%d\n", acc + split(n));
  return 0;
}
