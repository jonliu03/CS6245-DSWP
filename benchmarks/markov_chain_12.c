// markov_chain_12.c — Markov-chain Monte Carlo with chained per-sample
// observation transforms.
//
// At each step, a linear-congruential Markov chain advances `state`
// (cheap recurrence: state' = state * a + c). Each step produces a
// floating-point sample drawn from the chain's current state, and 12
// chained "observation" transforms are applied to that sample before
// accumulating the result. The observation chain has no cross-iteration
// dependence — obs_a..obs_l per iteration depend only on the current
// sample — so DSWP can pipeline the chain across iterations.
//
// Why this fits DSWP cleanly:
//   - cheap recurrence (LCG state update) → replicated iv chain
//   - heavy chain independent per iteration → partitioned across stages
//   - cheap accumulator (sum += l)         → reduction stage
// Same shape as llist_heavy_12, but framed around MCMC sampling
// instead of linked-list traversal: counted loop, scalar state walker,
// no Node/malloc.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#ifndef HEAVY_ITERS
#define HEAVY_ITERS 30
#endif

#define HEAVY_FN(name, mul1, mul2)                              \
  __attribute__((noinline, const))                              \
  double name(double x) {                                       \
    double acc = x;                                             \
    for (int i = 0; i < HEAVY_ITERS; ++i)                       \
      acc = sin(acc * (mul1)) + cos(acc * (mul2));              \
    return acc;                                                 \
  }

HEAVY_FN(obs_a, 1.00, 0.97)
HEAVY_FN(obs_b, 1.01, 0.99)
HEAVY_FN(obs_c, 1.03, 0.95)
HEAVY_FN(obs_d, 0.98, 1.02)
HEAVY_FN(obs_e, 1.05, 0.93)
HEAVY_FN(obs_f, 0.95, 1.04)
HEAVY_FN(obs_g, 1.07, 0.92)
HEAVY_FN(obs_h, 0.94, 1.06)
HEAVY_FN(obs_i, 1.09, 0.90)
HEAVY_FN(obs_j, 0.92, 1.08)
HEAVY_FN(obs_k, 1.11, 0.88)
HEAVY_FN(obs_l, 0.90, 1.10)

// Returns the raw sum (not the mean). The caller divides by n_samples
// outside this function — keeping the divide *inside* would make the
// return value an fdiv on top of an LCSSA phi, which our pass's
// live-out tracing doesn't yet follow.
__attribute__((noinline))
double markov_chain_sum(uint64_t seed, long n_samples) {
  uint64_t state = seed;
  double sum = 0.0;
  for (long s = 0; s < n_samples; s++) {
    // Advance the LCG Markov chain (cheap recurrence — replicated
    // across all DSWP stages so each thread iterates independently).
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    // Map state to a sample in [0, 1).
    double x = (double)(state >> 11) * (1.0 / (double)(1ULL << 53));
    // 12 chained observation transforms, each ~30 sin/cos iters.
    double a = obs_a(x);
    double b = obs_b(a);
    double c = obs_c(b);
    double d = obs_d(c);
    double e = obs_e(d);
    double f = obs_f(e);
    double g = obs_g(f);
    double h = obs_h(g);
    double i = obs_i(h);
    double j = obs_j(i);
    double k = obs_k(j);
    double l = obs_l(k);
    sum += l;
  }
  return sum;
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
  long n = (argc > 1) ? atol(argv[1]) : 100000;

  double t0 = now_sec();
  double s = markov_chain_sum(0xdeadbeefULL, n);
  double t1 = now_sec();

  printf("sum = %.6f  (%.4f s, n=%ld)\n", s, t1 - t0, n);
  return 0;
}
