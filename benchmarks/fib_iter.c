// fib_iter.c — iterative Fibonacci.
//
// Stress case for the upper-bound estimator. The analyzer DOES find a
// 2-stage partition (loop-counter SCC vs. Fibonacci-recurrence SCC are
// data-independent), but the partition is a no-op in practice: both
// SCCs are sequential and produce no values for each other. The lone
// cross-stage edge carries an iteration-continue token, so there is
// nothing to overlap. The analyzer reports a misleading speedup upper
// bound (~1.67x) because its cost model assumes any partition with
// non-trivial cross-stage data flow yields parallelism. A more honest
// estimator would discount partitions whose stages have no producer-
// consumer chain — left as future work.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

__attribute__((noinline))
long fib_iter(long n) {
  long a = 0, b = 1;
  for (long i = 0; i < n; ++i) {
    long c = a + b;
    a = b;
    b = c;
  }
  return a;
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
  long n = (argc > 1) ? atol(argv[1]) : 92;
  double t0 = now_sec();
  long r = fib_iter(n);
  double t1 = now_sec();
  printf("fib_n%ld = %ld  (%.4f s)\n", n, r, t1 - t0);
  return 0;
}
