// llist_heavy_12.c — 12 chained heavy compute calls per node.
//
// Larger version of llist_heavy.c, intended for many-core machines
// (e.g. PACE ICE cluster) where DSWP has room to scale to N=12 stages.
// Same recipe as llist_heavy.c: each heavy_X is noinline + const so
// the partitioner sees them as atomic, splittable units of work.

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
HEAVY_FN(heavy_f, 0.95, 1.04)
HEAVY_FN(heavy_g, 1.07, 0.92)
HEAVY_FN(heavy_h, 0.94, 1.06)
HEAVY_FN(heavy_i, 1.09, 0.90)
HEAVY_FN(heavy_j, 0.92, 1.08)
HEAVY_FN(heavy_k, 1.11, 0.88)
HEAVY_FN(heavy_l, 0.90, 1.10)

__attribute__((noinline))
double sum_heavy_12(Node *head) {
  double sum = 0.0;
  for (Node *p = head; p != NULL; p = p->next) {
    double a = heavy_a(p->value);
    double b = heavy_b(a);
    double c = heavy_c(b);
    double d = heavy_d(c);
    double e = heavy_e(d);
    double f = heavy_f(e);
    double g = heavy_g(f);
    double h = heavy_h(g);
    double i = heavy_i(h);
    double j = heavy_j(i);
    double k = heavy_k(j);
    double l = heavy_l(k);
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
  Node *head = build_list(n);

  double t0 = now_sec();
  double s = sum_heavy_12(head);
  double t1 = now_sec();

  printf("sum = %.6f  (%.4f s, n=%ld)\n", s, t1 - t0, n);
  free_list(head);
  return 0;
}
