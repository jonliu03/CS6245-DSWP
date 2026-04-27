// dswp_stages_sum_list.c — externally-linked stage functions for the
// transformed sum_list. The DSWPTransform pass replaces sum_list's body
// with a driver that calls these via pthread_create. Eventually the
// transform pass will generate the stage bodies as IR, but for now we
// stay focused on validating the driver-rewrite machinery.
//
// Layout of `sum_list_args` MUST match what DSWPTransform.cpp builds:
//   { Node *head, long sum_out, dswp_queue *q }

#include "dswp_queue.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct Node {
  long value;
  struct Node *next;
} Node;

struct sum_list_args {
  Node *head;
  long sum_out;
  dswp_queue *q;
};

void *dswp_stage0_sum_list(void *raw) {
  struct sum_list_args *a = (struct sum_list_args *)raw;
  for (Node *p = a->head; p != NULL; p = p->next)
    dswp_enqueue(a->q, (uint64_t)p->value);
  dswp_send_eof(a->q);
  return NULL;
}

void *dswp_stage1_sum_list(void *raw) {
  struct sum_list_args *a = (struct sum_list_args *)raw;
  long sum = 0;
  uint64_t v;
  while (dswp_dequeue(a->q, &v))
    sum += (long)v;
  a->sum_out = sum;
  return NULL;
}
