#include <stdio.h>

volatile int a[1024];

__attribute__((noinline, noclone))
int test_unroll(void) {
  int sum = 0;
  int i, j;

  for (i = 0; i < 32; i++) {
      for (j = 0; j < 4; j++)
	sum += a[i + j*4] * i;

  }

  return sum;
}

float vx[2048];
float vy[2048];
float vz[2048];

/* SIMD-friendly loop */
__attribute__((noinline, noclone))
float test_vector_loop(void) {
  int j;
  for (j = 0; j < 2048; j++) {
    vz[j] = vx[j] + vy[j];
  }
  return vz[100];
}

int main(void) {
  volatile int sink_i = 0;
  volatile float sink_f = 0.f;
  for (int j = 0; j < 100000; j++) {
    sink_i += test_unroll();
    sink_f += test_vector_loop();
  }
  printf("%d %g\n", sink_i, (double)sink_f);
  return 0;
}
