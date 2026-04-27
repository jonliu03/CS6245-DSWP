// llist_sum.c — minimal linked-list-traversal benchmark for DSWP analysis.
//
// The hot loop has two SCCs in its PDG:
//   1) the pointer-chase recurrence (p = p->next)         ← cannot be split
//   2) the running sum                                    ← depends on (1)
// A 2-stage DSWP partition should split these.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct Node {
  long value;
  struct Node *next;
} Node;

static Node *build_list(long n) {
  Node *head = NULL;
  for (long i = n; i > 0; --i) {
    Node *node = (Node *)malloc(sizeof(Node));
    node->value = i;
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

// The hot loop. Marked noinline so it survives -O1 as its own function and
// the analyzer / transform pass can find it deterministically.
__attribute__((noinline))
long sum_list(Node *head) {
  long sum = 0;
  for (Node *p = head; p != NULL; p = p->next) {
    sum += p->value;
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
  long s = sum_list(head);
  double t1 = now_sec();
  printf("sum = %ld  (%.4f s, n=%ld)\n", s, t1 - t0, n);
  free_list(head);
  return 0;
}
