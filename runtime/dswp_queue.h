/*
 * DSWP runtime SPSC ring buffer.
 *
 * One producer thread, one consumer thread. Lock-free; producer blocks on
 * full, consumer blocks on empty. EOF is signaled separately so the
 * consumer can drain remaining items and then terminate cleanly.
 *
 * C ABI so the eventual LLVM-emitted IR can call this without name mangling.
 */

#ifndef DSWP_QUEUE_H
#define DSWP_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dswp_queue dswp_queue;

/* capacity is rounded up to the next power of 2 (and to at least 8 slots). */
dswp_queue *dswp_queue_create(size_t capacity);
void        dswp_queue_destroy(dswp_queue *q);

/* Producer. Blocks (yields) while the queue is full. */
void dswp_enqueue(dswp_queue *q, uint64_t value);

/* Producer signals end-of-stream after its final enqueue. */
void dswp_send_eof(dswp_queue *q);

/*
 * Consumer. Returns true and writes *out if a value was dequeued; returns
 * false when the queue is permanently empty (EOF was sent by producer and
 * all enqueued items have been drained).
 */
bool dswp_dequeue(dswp_queue *q, uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSWP_QUEUE_H */
