// max_list.c — find the max value in a linked list.
//
// Same loop shape as sum_list but with `max(sum, v)` instead of
// `sum + v`. Used to verify that the generic 2-stage transform pass
// generalizes to different reduction operations without per-function
// code.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct Node {
  long value;
  struct Node *next;
} Node;

static Node *build_list(long n) {
  Node *head = NULL;
  for (long i = 1; i <= n; ++i) {
    Node *node = (Node *)malloc(sizeof(Node));
    // Pseudo-random values so the max isn't trivially the last node.
    node->value = (long)((i * 2654435761ULL) % 1000000);
    node->next = head;
    head = node;
  }
  return head;
}

static void free_list(Node *head) {
  while (head) { Node *nx = head->next; free(head); head = nx; }
}

__attribute__((noinline))
long max_list(Node *head) {
  long best = -1;
  for (Node *p = head; p != NULL; p = p->next) {
    long v = p->value;
    if (v > best) best = v;
  }
  return best;
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
  long m = max_list(head);
  double t1 = now_sec();
  printf("max = %ld  (%.4f s, n=%ld)\n", m, t1 - t0, n);
  free_list(head);
  return 0;
}
