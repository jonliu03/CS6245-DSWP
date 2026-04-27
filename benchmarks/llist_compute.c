// llist_compute.c — linked-list walk with expensive per-node compute.
//
// This is the canonical DSWP showcase: the pointer chase is fast and
// memory-bound (stage 0), while the per-node math is compute-bound (stage 1).
// Splitting them lets stage 0 prefetch ahead of stage 1.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct Node {
  double value;
  struct Node *next;
} Node;

static Node *build_list(long n) {
  Node *head = NULL;
  for (long i = n; i > 0; --i) {
    Node *node = (Node *)malloc(sizeof(Node));
    node->value = (double)i;
    node->next = head;
    head = node;
  }
  return head;
}

static void free_list(Node *head) {
  while (head) {
    Node *next = head->next;
    free(head);
    head = next;
  }
}

__attribute__((noinline))
double sum_with_compute(Node *head) {
  double sum = 0.0;
  for (Node *p = head; p != NULL; p = p->next) {
    double v = p->value;
    // Heavy per-node compute. sqrt and log are typically marked readnone
    // so they do not introduce memory deps in the PDG.
    sum += sqrt(v) + log(v + 1.0);
  }
  return sum;
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
  long n = (argc > 1) ? atol(argv[1]) : 1000000;
  Node *head = build_list(n);
  double t0 = now_sec();
  double s = sum_with_compute(head);
  double t1 = now_sec();
  printf("sum = %.4f  (%.4f s, n=%ld)\n", s, t1 - t0, n);
  free_list(head);
  return 0;
}
