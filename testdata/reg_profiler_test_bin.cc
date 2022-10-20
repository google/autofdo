#include <cstdio>
#include <cstdlib>

extern "C" {
__attribute__((noinline))
int test(int t) {
  int k = 0;
  for (int i = 1; i <= t; ++i) {
    k += i;
  }
  printf("k = %d\n", k);
  return 0;
}
}

int main(int argc, char** argv) {
  return test(atoi(argv[1]));
}
