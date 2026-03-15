# Design Document

This document explains the *why* behind every major design decision in this project. It's intended for engineers reviewing the code or interviewers asking about tradeoffs.

---

## Table of Contents

1. [Why Lock-Free Over Mutex-Based Queues](#1-why-lock-free-over-mutex-based-queues)
2. [Memory Ordering: Why acquire/release Instead of seq_cst](#2-memory-ordering-why-acquirerelease-instead-of-seq_cst)
3. [Why Single Producer](#3-why-single-producer)
4. [Ring Buffer: Why Power-of-Two Sizing](#4-ring-buffer-why-power-of-two-sizing)
5. [Cache-Line Alignment: Preventing False Sharing](#5-cache-line-alignment-preventing-false-sharing)
6. [Why Discriminated Unions Over std::variant or Inheritance](#6-why-discriminated-unions-over-stdvariant-or-inheritance)
7. [Lazy Minimum-Sequence Caching](#7-lazy-minimum-sequence-caching)
8. [Wait Strategy Design: Why Three Strategies](#8-wait-strategy-design-why-three-strategies)
9. [Why eventfd Over futex or Condition Variables](#9-why-eventfd-over-futex-or-condition-variables)
10. [RAII Shared Memory: What the Original Got Wrong](#10-raii-shared-memory-what-the-original-got-wrong)
11. [Latency Measurement: Why Percentiles Over Averages](#11-latency-measurement-why-percentiles-over-averages)
12. [Graceful Shutdown: Why It Matters](#12-graceful-shutdown-why-it-matters)
13. [Extending to Multi-Producer](#13-extending-to-multi-producer)

---

## 1. Why Lock-Free Over Mutex-Based Queues

A mutex-based queue has three problems in the IPC/low-latency context:

**Kernel transitions.** `pthread_mutex_lock` / `unlock` may invoke the `futex` syscall, transitioning from user space to kernel space. Each transition costs ~1-5μs — more than our entire target latency budget.

**Priority inversion.** If a low-priority consumer holds the lock and gets preempted by the OS scheduler, all other consumers and the producer are blocked until the scheduler reschedules that thread. Latency becomes non-deterministic.

**Cache line bouncing.** A mutex is typically a single cache line that every thread contends on. Every lock/unlock causes that cache line to bounce between cores via the MESI protocol, generating coherency traffic that pollutes the cache.

Our lock-free design eliminates all three: no syscalls in the hot path, no blocking, and each consumer reads its own cache-line-isolated sequence counter.

**Tradeoff:** Lock-free code is harder to reason about correctness. We accept this complexity in exchange for deterministic latency.

---

## 2. Memory Ordering: Why acquire/release Instead of seq_cst

C++ provides several memory orderings. The two relevant choices here:

**`memory_order_seq_cst` (sequential consistency):** The default. Guarantees a single total order of all atomic operations visible to all threads. Implemented on x86 via `MFENCE` or `LOCK`-prefixed instructions, which are expensive (~20-50 cycles).

**`memory_order_acquire` / `memory_order_release`:** Weaker but sufficient. A release store guarantees that all prior writes are visible to any thread that subsequently does an acquire load of the same variable.

Our publish/consume protocol needs exactly one guarantee: "if the consumer sees the updated sequence number, it must also see the message data that was written before it." This is precisely what acquire/release provides.

The coordination pattern:

```
Producer:                           Consumer:
  slot.data = item;                   if (slot.sequence.load(acquire) > seq) {
  slot.sequence.store(release);           out = slot.data;  // guaranteed visible
```

The release store on `slot.sequence` synchronizes-with the acquire load. Everything written before the store (the message data) is guaranteed visible after the load. No `MFENCE` needed.

**On x86-64 specifically:** acquire loads compile to plain `MOV` instructions (x86 has strong memory ordering by default — all loads are acquire loads). Release stores also compile to plain `MOV` (all stores are release stores on x86). So on our target platform, acquire/release is literally free — zero additional instructions compared to relaxed. But writing the code with correct orderings matters for:
1. Correctness on ARM/POWER if we ever port
2. Communicating intent to other developers
3. Preventing the compiler from reordering across the atomic

**Why not relaxed everywhere?** `memory_order_relaxed` gives no ordering guarantees. The compiler (not just the CPU) is free to reorder a relaxed store of the sequence before the write to `slot.data`, which would let consumers read garbage. acquire/release prevents this.

---

## 3. Why Single Producer

The queue is designed for Single Producer, Multiple Consumer (SPMC). This is a deliberate constraint, not a limitation.

**Simplicity:** With one producer, the `next_` counter doesn't need to be atomic. The producer simply does `next_ = next_seq` — no CAS loop, no contention, no retry logic. This is one less atomic operation per publish.

**Deterministic ordering:** A single producer creates a natural total order of messages. Every consumer sees messages in exactly the same sequence. With multiple producers, you'd need either a CAS-based sequence claiming mechanism (adding latency) or per-producer queues with consumer-side merging (adding complexity).

**Matches the domain:** In trading systems, the market data feed is inherently a single stream. One feed handler process normalizes data and publishes it. Multiple strategy engines consume it. The SPMC pattern fits naturally.

See [Section 13](#13-extending-to-multi-producer) for how to extend to multi-producer if needed.

---

## 4. Ring Buffer: Why Power-of-Two Sizing

The ring buffer has `kRingSize = 1024` slots. The power-of-two constraint enables:

```cpp
index = sequence & (kRingSize - 1);   // bitwise AND
// instead of:
index = sequence % kRingSize;          // division
```

On x86, the modulo operator compiles to a `DIV` instruction (~20-30 cycles). Bitwise AND compiles to a single `AND` instruction (~1 cycle). Since index computation happens on every publish and every consume, this saves ~20 cycles per operation.

**Why 1024?** This gives ~1024 messages of buffer between the producer and the slowest consumer. At 1M messages/second, that's ~1ms of slack. Too small and the producer hits backpressure frequently. Too large and you waste memory (each slot is 64+ bytes, so 1024 slots ≈ 64KB — fits in L1 cache on most modern CPUs).

The size is a compile-time constant (`constexpr`) so the compiler can optimize the AND operation into an immediate operand.

---

## 5. Cache-Line Alignment: Preventing False Sharing

Every `Slot`, `ConsumerSlot`, and `Sequence` is `alignas(64)`.

**What is false sharing?** Modern CPUs don't read individual bytes from memory — they read entire cache lines (64 bytes on x86). If two threads are writing to different variables that happen to be on the same cache line, the CPU's cache coherency protocol (MESI) forces the cache line to bounce between cores. Each bounce costs ~50-100 cycles.

**Where it matters in this queue:**

1. **Between adjacent Slots:** Without alignment, `ring_[0].sequence` and `ring_[1].data` could share a cache line. The producer writing to slot 1 would invalidate the consumer reading slot 0.

2. **Between ConsumerSlots:** Without alignment, Consumer 0's sequence and Consumer 1's sequence could share a line. Consumer 0 advancing its sequence would force Consumer 1 to re-fetch from main memory.

3. **Between the producer's `next_` and consumer sequences:** These are on different cache lines because the `ring_` and `consumers_` arrays are separately aligned.

**Cost:** Each aligned struct wastes up to 63 bytes of padding. For 64 ConsumerSlots, that's ~4KB of padding — negligible on modern systems and well worth the performance gain.

---

## 6. Why Discriminated Unions Over std::variant or Inheritance

The `Message` struct uses a manual tagged union:

```cpp
struct Message {
    MessageType type;       // tag
    uint64_t timestamp_ns;
    union { AddOrder add; Trade trade; DeleteOrder del; };
};
```

**Alternative 1: `std::variant<AddOrder, Trade, DeleteOrder>`**

`std::variant` adds:
- Runtime type index checking on every `std::visit` or `std::get`
- Exception handling for `valueless_by_exception` state
- 8 bytes of overhead for the type index (implementations typically use `size_t`)
- `std::visit` uses function pointer dispatch, which is an indirect call — harder for branch prediction

Our switch-case on `MessageType` compiles to a direct jump table — no indirection.

**Alternative 2: Inheritance with virtual functions**

Virtual dispatch adds:
- 8 bytes for the vtable pointer per message
- Indirect function call through the vtable on every dispatch
- **Critical:** vtable pointers are process-local addresses. In shared memory, Process A's vtable is at a different address than Process B's vtable. A virtual call in Process B using a message written by Process A would dereference a dangling pointer. **This is a hard blocker for shared memory.**

**Alternative 3: Our tagged union**

- 1 byte for the enum tag
- No indirection — switch/case compiles to a jump table
- Trivially copyable — safe for `memcpy` across shared memory
- Entire message fits in one cache line (~40 bytes)
- Zero overhead compared to a raw struct

---

## 7. Lazy Minimum-Sequence Caching

The producer must check that no consumer is lagging so far behind that the ring would wrap and overwrite unread data. The naive approach:

```cpp
// On every publish: scan all 64 consumers
uint64_t min = UINT64_MAX;
for (int i = 0; i < 64; ++i) {
    if (consumers_[i].active) min = std::min(min, consumers_[i].sequence);
}
```

This is O(64) per publish — 64 cache-line reads, potentially crossing NUMA boundaries.

**Our optimization:** Cache `min_consumer_seq_` and only rescan when the fast check suggests the ring is full:

```cpp
if ((next_seq - min_consumer_seq_) >= kRingSize) [[unlikely]] {
    // Rescan only here — this is the slow path
    min_consumer_seq_ = actual_minimum();
}
```

The `[[unlikely]]` attribute tells the compiler to optimize the fast path (no rescan) and move the slow path code out of line. In practice, if consumers keep up with the producer, the rescan happens very rarely — perhaps once per full ring traversal (every 1024 messages), amortizing the O(64) cost over 1024 publishes.

---

## 8. Wait Strategy Design: Why Three Strategies

The original implementation had one approach: busy-spin forever. This is optimal for exactly one scenario (dedicated core, maximum throughput) and wasteful for everything else.

Real systems need different tradeoffs depending on the consumer's role:

| Consumer Role | Latency Requirement | CPU Budget | Best Strategy |
|---|---|---|---|
| Matching engine | Sub-microsecond | Dedicated core | BusySpin |
| Risk monitor | Low microseconds | Shared cores | Yielding |
| Trade logger | Milliseconds OK | Minimal CPU | Blocking |

**Why a Strategy pattern (runtime polymorphism) instead of compile-time templates?**

Templates would eliminate the virtual call overhead (~2ns) but would require separate binaries or complex template instantiation for each strategy. Since the wait strategy is called only when there's NO data (i.e., we're already idle), the virtual dispatch cost is negligible compared to the yield/sleep cost. Runtime selection lets users pick a strategy via command-line argument without recompiling.

**The three-phase progression within Yielding and Blocking:**

Both strategies start with a spin phase before falling back. This handles the common case: the producer is a few nanoseconds behind, and data arrives during the spin. Only if the spin phase exhausts without data do we escalate to yielding or sleeping. This hybrid approach captures the best of both worlds — fast response when data is close, low CPU when it's not.

---

## 9. Why eventfd Over futex or Condition Variables

**Condition variables (`std::condition_variable`):**
- Require a mutex (we're lock-free — adding a mutex defeats the purpose)
- Can't be used across processes via shared memory without careful setup
- Spurious wakeups require a predicate loop

**`futex` (Fast Userspace Mutex):**
- Powerful but low-level — requires careful handling of edge cases
- `FUTEX_WAKE` wakes a specific number of waiters, not all
- Operates on a 32-bit integer in shared memory, which is good for IPC
- But the API is complex and error-prone

**`eventfd`:**
- Simple: one `write()` to signal, one `poll()` + `read()` to wait
- Integrates with `poll()`/`epoll()` — consumers can watch multiple fds
- `EFD_SEMAPHORE` mode: each consumer independently drains the counter
- A single `write(fd, 1)` wakes ALL consumers blocked in `poll()`
- Clean, well-documented API with minimal edge cases

**Tradeoff:** `eventfd` is Linux-specific. For portability, `futex` or a pipe-based approach would be needed. Since the entire project is Linux-specific (shared memory, `_mm_pause`, `__rdtsc`), `eventfd` fits naturally.

**Note on IPC:** In the current implementation, each process creates its own `eventfd`. For true cross-process blocking notification, the `eventfd` would need to be created once and the file descriptor passed to consumers via `SCM_RIGHTS` (Unix domain socket fd passing) or stored as a known path. This is a simplification for the demo — the architecture supports it, but the fd-sharing plumbing is not yet implemented.

---

## 10. RAII Shared Memory: What the Original Got Wrong

The original `map_shared_queue()` function had several issues:

```cpp
// Original code (simplified):
int fd = shm_open(name, flags, 0666);
ftruncate(fd, size);                    // fd leaked if this fails
void* ptr = mmap(nullptr, size, ...);   // fd still leaked
return reinterpret_cast<Queue*>(ptr);   // fd NEVER closed
                                         // ptr NEVER munmap'd
                                         // UB: no placement new for consumers
```

**Problems:**
1. `fd` is never `close()`d. File descriptors are a limited resource (~1024 default).
2. If `ftruncate` fails, `fd` leaks.
3. If `mmap` fails, `fd` leaks.
4. The returned pointer is never `munmap()`'d on exit.
5. Consumer processes use `reinterpret_cast` on memory that was constructed by a different process. Without placement `new`, accessing the object is technically undefined behavior in C++ (no object lifetime has begun in the consumer's context).

**Our SharedMemoryManager fixes:**
1. `fd` is closed immediately after `mmap` (the mapping holds its own reference).
2. Every failure path cleans up: close fd, unlink shm, unmap memory.
3. Destructor guarantees `munmap` and (if owner) `shm_unlink`.
4. Producer uses placement `new` to construct the queue in shared memory.
5. Move semantics supported; copy prevented (deleted copy constructor).

This is textbook RAII — resource acquisition in the constructor, release in the destructor, exception-safe error handling.

---

## 11. Latency Measurement: Why Percentiles Over Averages

The original code reported only average latency:

```cpp
double avg_latency = total_latency / count;
```

Average latency is misleading because it hides the distribution. Consider:
- 999,000 messages at 0.1μs + 1,000 messages at 100μs = average 0.2μs
- But 0.1% of messages took 1000x longer

In trading, a strategy that's fast 99% of the time but slow 1% of the time will lose money on those 1% of trades. **p99 latency** — the latency that 99% of messages are faster than — is the metric that matters.

Our `LatencyTracker` pre-allocates storage for all samples (no heap allocation during measurement), records raw cycle counts in the hot path (one `vector::push_back`), and sorts + computes percentiles only at report time.

---

## 12. Graceful Shutdown: Why It Matters

The original consumer loop:

```cpp
while (count < kTotalMessages) {
    if (!queue->consume(cid, msg)) {
        _mm_pause();
        continue;   // spins forever if producer dies
    }
}
```

If the producer crashes, exits early, or publishes fewer than `kTotalMessages`, the consumer hangs indefinitely. In a BusySpin strategy, it burns 100% CPU forever.

Our shutdown protocol:
1. Producer sets an atomic `shutdown_flag_` and calls `notify_all()` to wake sleeping consumers.
2. Consumers periodically check the flag during idle periods.
3. On shutdown, consumers drain any remaining messages before exiting.

This ensures clean exit in all scenarios: normal completion, early termination, producer crash (via external signal handler setting the flag).

---

## 13. Extending to Multi-Producer

The current design is single-producer. To support multiple producers:

**Change 1:** Make `next_` atomic and use CAS to claim sequence numbers:

```cpp
uint64_t next_seq;
do {
    next_seq = next_.load(std::memory_order_relaxed);
} while (!next_.compare_exchange_weak(
    next_seq, next_seq + 1, std::memory_order_acq_rel));
```

Each producer atomically claims the next sequence number. Only one wins per slot.

**Change 2:** Two-phase publish. After claiming a sequence, the producer writes data, then signals completion. Other producers may have claimed later sequences but not finished writing — consumers must wait for all prior sequences to be published before reading.

**Change 3:** `min_consumer_seq_` must become atomic since multiple producers read and update it.

**Cost:** The CAS loop adds ~10-50ns per publish under contention. For most use cases, the single-producer design with a dedicated feed handler process is simpler and faster.
