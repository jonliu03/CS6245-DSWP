// SPSC ring buffer for the DSWP runtime. Single-producer, single-consumer.
//
// Layout puts head and tail on separate cache lines so producer and
// consumer don't ping-pong the same line. EOF is a separate atomic flag
// signaled after the last enqueue; the consumer drains remaining items
// and then terminates.

#include "dswp_queue.h"

#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

namespace {

constexpr size_t CACHE_LINE = 64;

inline size_t roundUpPow2(size_t n) {
  size_t p = 8;
  while (p < n) p <<= 1;
  return p;
}

inline size_t roundUp(size_t v, size_t align) {
  return (v + align - 1) & ~(align - 1);
}

} // namespace

struct dswp_queue {
  // Producer-touched (write head, read tail to check fullness).
  alignas(CACHE_LINE) std::atomic<size_t> head;
  // Consumer-touched (write tail, read head to check emptiness).
  alignas(CACHE_LINE) std::atomic<size_t> tail;
  // EOF flag. Set by producer after final enqueue; read by consumer
  // when the queue appears empty.
  alignas(CACHE_LINE) std::atomic<bool> eof;
  // Read-mostly fields.
  alignas(CACHE_LINE) size_t capacity;  // power of 2
  size_t   mask;
  uint64_t *buffer;
};

dswp_queue *dswp_queue_create(size_t capacity) {
  size_t cap = roundUpPow2(capacity);
  size_t struct_bytes = roundUp(sizeof(dswp_queue), CACHE_LINE);
  void *mem = std::aligned_alloc(CACHE_LINE, struct_bytes);
  if (!mem) return nullptr;
  auto *q = new (mem) dswp_queue();
  q->head.store(0, std::memory_order_relaxed);
  q->tail.store(0, std::memory_order_relaxed);
  q->eof.store(false, std::memory_order_relaxed);
  q->capacity = cap;
  q->mask = cap - 1;
  size_t buf_bytes = roundUp(sizeof(uint64_t) * cap, CACHE_LINE);
  q->buffer = static_cast<uint64_t *>(std::aligned_alloc(CACHE_LINE, buf_bytes));
  if (!q->buffer) { std::free(q); return nullptr; }
  return q;
}

void dswp_queue_destroy(dswp_queue *q) {
  if (!q) return;
  std::free(q->buffer);
  q->~dswp_queue();
  std::free(q);
}

void dswp_enqueue(dswp_queue *q, uint64_t value) {
  // We are the only producer, so reading head with relaxed is fine —
  // nothing else writes it.
  size_t h = q->head.load(std::memory_order_relaxed);
  // Wait for room. The release on the consumer's tail.store synchronizes
  // with this acquire load.
  while (h - q->tail.load(std::memory_order_acquire) >= q->capacity) {
    std::this_thread::yield();
  }
  q->buffer[h & q->mask] = value;
  // Release: makes the buffer write visible before the consumer sees the
  // new head value.
  q->head.store(h + 1, std::memory_order_release);
}

void dswp_send_eof(dswp_queue *q) {
  // Release: ensures all prior enqueues are visible before the consumer
  // observes EOF.
  q->eof.store(true, std::memory_order_release);
}

bool dswp_dequeue(dswp_queue *q, uint64_t *out) {
  size_t t = q->tail.load(std::memory_order_relaxed);
  for (;;) {
    size_t h = q->head.load(std::memory_order_acquire);
    if (h != t) {
      *out = q->buffer[t & q->mask];
      q->tail.store(t + 1, std::memory_order_release);
      return true;
    }
    // Queue appears empty. Check EOF, then re-check head: the producer
    // might have enqueued one final value before setting EOF, and the
    // acquire on eof guarantees the head update is visible if so.
    if (q->eof.load(std::memory_order_acquire)) {
      size_t h2 = q->head.load(std::memory_order_acquire);
      if (h2 == t) return false;
      // Otherwise loop and dequeue the straggler.
    } else {
      std::this_thread::yield();
    }
  }
}
