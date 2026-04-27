// llist_heavy.c — linked-list walk with N chained heavy compute calls
// per node. The DSWP cloned transform partitions these calls (and the
// walker / accumulator) across DSWP_NUM_STAGES stages.
//
// Five heavy functions (heavy_a..heavy_e) are wired in fixed order so
// that the analyzer always sees the same body. The PASS decides how
// many stages to split into — see scripts/run_stage_sweep.sh which
// runs N = 1, 2, 3, 4, 5.
//
// All heavy functions are noinline + const so:
//   - noinline: keeps each call as a single IR instruction (else they'd
//     dissolve into the outer loop and the partition would collapse)
//   - const:    tells the analyzer they have no memory side effects, so
//     PDG memory edges don't merge their SCCs into one mega-SCC
//
// Each function does HEAVY_ITERS rounds of sin+cos with a slightly
// different coefficient — enough work per call (~hundreds of cycles)
// that queue overhead is amortized.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef HEAVY_ITERS
#define HEAVY_ITERS 30
#endif

typedef struct Node {
  double value;
  struct Node *next;
} Node;

static Node *build_list(long n) {
  Node *head = NULL;
  for (long i = n; i > 0; --i) {
    Node *node = (Node *)malloc(sizeof(Node));
    node->value = (double)i + 0.5;
    node->next = head;
    head = node;
  }
  return head;
}

static void free_list(Node *head) {
  while (head) { Node *nx = head->next; free(head); head = nx; }
}

#define HEAVY_FN(name, mul1, mul2)                              \
  __attribute__((noinline, const))                              \
  double name(double x) {                                       \
    double acc = x;                                             \
    for (int i = 0; i < HEAVY_ITERS; ++i)                       \
      acc = sin(acc * (mul1)) + cos(acc * (mul2));              \
    return acc;                                                 \
  }

HEAVY_FN(heavy_a, 1.00, 0.97)
HEAVY_FN(heavy_b, 1.01, 0.99)
HEAVY_FN(heavy_c, 1.03, 0.95)
HEAVY_FN(heavy_d, 0.98, 1.02)
HEAVY_FN(heavy_e, 1.05, 0.93)

__attribute__((noinline))
double sum_heavy(Node *head) {
  double sum = 0.0;
  for (Node *p = head; p != NULL; p = p->next) {
    double a = heavy_a(p->value);
    double b = heavy_b(a);
    double c = heavy_c(b);
    double d = heavy_d(c);
    double e = heavy_e(d);
    sum += e;
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
  Node *head = build_list(n);

  double t0 = now_sec();
  double s = sum_heavy(head);
  double t1 = now_sec();

  printf("sum = %.6f  (%.4f s, n=%ld)\n", s, t1 - t0, n);
  free_list(head);
  return 0;
}
