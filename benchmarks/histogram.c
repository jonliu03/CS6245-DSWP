// histogram.c — array-driven histogram with memory recurrence.
//
// The hot loop reads `data[i]`, computes a bin index, then RMWs `bins[idx]`.
// Cross-iteration store-load on `bins` produces a loop-carried memory edge
// (since two iterations may hash to the same bin). This stresses the PDG's
// memory-dependence handling (DependenceInfo + MemorySSA) — distinct from
// the register-data recurrences in llist_*.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NBINS 256

__attribute__((noinline))
void histogram(const int *data, long n, long *bins) {
  for (long i = 0; i < n; ++i) {
    int idx = data[i] & (NBINS - 1);
    bins[idx] += 1;
  }
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
  long n = (argc > 1) ? atol(argv[1]) : 1000000;
  int *data = (int *)malloc(sizeof(int) * n);
  long bins[NBINS] = {0};
  // Cheap pseudo-random fill.
  unsigned long s = 0xdeadbeef;
  for (long i = 0; i < n; ++i) {
    s = s * 6364136223846793005ULL + 1;
    data[i] = (int)(s >> 32);
  }

  double t0 = now_sec();
  histogram(data, n, bins);
  double t1 = now_sec();

  long total = 0;
  for (int i = 0; i < NBINS; ++i) total += bins[i];
  printf("total = %ld  (%.4f s, n=%ld)\n", total, t1 - t0, n);
  free(data);
  return 0;
}
