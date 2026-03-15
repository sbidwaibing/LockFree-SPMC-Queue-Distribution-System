# Lock-Free Shared Memory Queue for Low-Latency Inter-Process Pub-Sub with Adaptive Wait Strategies

A high-performance, single-producer multi-consumer (SPMC) publish-subscribe queue in C++ designed for ultra-low-latency inter-process communication via POSIX shared memory. Inspired by the [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/) pattern, this project implements lock-free coordination, cache-line-aligned data structures, and adaptive wait strategies to minimize both latency and CPU waste.

---

## Origin & My Contributions

The core lock-free SPMC ring buffer and shared memory mapping are inspired by [Manoj Piyumal De Silva's LockFreeQueueForIPC](https://github.com/manojds/LockFreeQueueForIPC), which implements a minimal single-producer multi-consumer queue based on the LMAX Disruptor pattern. That project demonstrated the foundational lock-free publish/consume mechanics with busy-spin polling.

**I extended the project significantly with the following:**

### Adaptive Wait Strategy System
The original implementation had a single mode: busy-spin forever, burning 100% CPU per consumer regardless of data flow. I designed and implemented a pluggable `WaitStrategy` interface using the **Strategy design pattern**, with three concrete implementations:
- **`BusySpinWaitStrategy`** — preserves the original ultra-low-latency behavior
- **`YieldingWaitStrategy`** — spins for a configurable number of iterations, then yields the thread via `std::this_thread::yield()`
- **`BlockingWaitStrategy`** — spins briefly, then sleeps on a Linux `eventfd`; the producer broadcasts a wakeup to all sleeping consumers when new data is published. This reduces idle CPU usage from 100% to near zero.

### `eventfd` Broadcast Notification
Implemented a kernel-level notification mechanism where a single `eventfd` write from the producer wakes all blocked consumers simultaneously — replacing the naive spin-poll loop with an event-driven architecture when latency tolerance allows it.

### OOP Restructuring
Refactored the original procedural codebase into proper classes with clear ownership and separation of concerns:
- **`Producer`** — encapsulates queue access, shared memory lifecycle, and the publish loop. Integrates `eventfd` signaling for `BlockingWaitStrategy` consumers.
- **`Consumer`** — base class with a virtual `onMessage()` hook, enabling polymorphic message handling (e.g., subclass into a `LoggingConsumer`, `MatchingEngineConsumer`, etc.). Owns its wait strategy and consumer registration lifecycle.
- **`SharedMemoryManager`** — RAII wrapper around `shm_open`, `ftruncate`, `mmap`, `munmap`, and `shm_unlink`. Eliminates the resource leaks present in the original code (unclosed file descriptors, unmapped memory on error paths).

### RAII Shared Memory Lifecycle
The original code used raw `shm_open`/`mmap` calls in a free function with no cleanup on failure paths — leaked file descriptors, no `munmap` on exit, and `reinterpret_cast` on mmap'd memory without placement `new` (undefined behavior for consumer processes). I replaced this with a `SharedMemoryManager` class that handles construction, destruction, and error recovery through RAII, making the code safe and deterministic.

### Latency Benchmarking with Percentile Reporting
Extended the original average-only latency measurement to report **p50, p95, p99, and max latencies** per consumer. Average latency hides tail spikes — in trading systems, p99 is what actually matters. Benchmarks are collected across all three wait strategies for direct comparison.

### Graceful Shutdown Protocol
The original consumers spin forever if the producer stops early or crashes. I added a shutdown mechanism using an atomic flag in the shared memory region that the producer sets on completion, allowing consumers to exit cleanly instead of hanging.

---

## The Problem

### For Everyone

Imagine a stock exchange where thousands of orders flood in every second. Multiple backend systems need to see every single order at the same time — the matching engine, the risk monitor, the trade logger. These systems run as **separate programs** (processes) on the same machine.

The challenge: **how do you get a message from one program to all the others as fast as physically possible?**

Traditional approaches like network sockets, pipes, or message queues all go through the operating system kernel, adding microseconds of delay per message. In trading, where a single microsecond can mean the difference between profit and loss, that overhead is unacceptable.

This project bypasses the OS entirely. It places a shared data structure directly in memory that all processes can read and write to — no kernel, no locks, no waiting in line. The result is message delivery in **hundreds of nanoseconds** instead of tens of microseconds.

But raw speed creates a new problem: if there's no data flowing, every consumer burns 100% of a CPU core just checking "is there a message yet?" over and over. This project solves that with **adaptive wait strategies** — consumers spin at full speed when data is flowing, then gracefully back off to sleeping when the stream goes quiet, waking up instantly when new data arrives.

### For Engineers

This is a **lock-free SPMC ring buffer** mapped into POSIX shared memory (`shm_open` + `mmap`) for zero-copy, zero-syscall IPC in the hot path. Key design decisions:

- **Atomic sequence-based coordination**: Each ring slot carries an atomic sequence number. The producer writes data then publishes via `store(release)`; consumers poll via `load(acquire)`. This acquire-release pairing establishes happens-before ordering without locks or fences.
- **Cache-line isolation**: Every `Slot`, `ConsumerSlot`, and `Sequence` is `alignas(64)` to eliminate false sharing. No two independent atomic variables share a cache line.
- **Bitwise index computation**: Ring size is constrained to a power of two, enabling `seq & (size - 1)` instead of modulo — a single AND instruction with no branch.
- **Lazy minimum-sequence caching**: The producer caches `min_consumer_seq_` and only rescans all consumers when the fast-path check suggests the ring is full, amortizing the O(n) scan.
- **Discriminated union messages**: A `MessageType` enum + anonymous union avoids vtable indirection, `std::variant` overhead, and dynamic allocation. The `Message` struct fits in a single cache line and is trivially copyable across process boundaries.
- **Adaptive wait strategies**: Pluggable `WaitStrategy` interface (`BusySpin`, `Yielding`, `Blocking`) allows consumers to trade latency for CPU efficiency depending on deployment requirements.

---

## Architecture

```
                    Shared Memory (mmap)
                    ┌──────────────────────────────────────────┐
                    │          Lock-Free Ring Buffer           │
                    │  ┌─────┬─────┬─────┬─────┬─────┬─────┐   │
  Producer ────────▶│  │Slot0│Slot1│Slot2│Slot3│ ... │S1023│   │
  (single writer)   │  └─────┴─────┴─────┴─────┴─────┴─────┘   │
                    │                                          │
                    │  ┌───────────────────────────────────┐   │
                    │  │ Consumer Slots (up to 64)         │   │
                    │  │ [seq][seq][seq]...                │   │
                    │  └───────────────────────────────────┘   │
                    └──────────┬───────────┬───────────┬───────┘
                               │           │           │
                          Consumer A   Consumer B   Consumer C
                          (process)    (process)    (process)
                               │           │           │
                          ┌────┴───┐  ┌────┴───┐  ┌────┴───┐
                          │ Wait   │  │ Wait   │  │ Wait   │
                          │Strategy│  │Strategy│  │Strategy│
                          └────────┘  └────────┘  └────────┘
```

### Data Flow

1. **Producer** writes a message to the next ring slot and atomically updates the slot's sequence number.
2. **Each consumer** independently reads from the ring at its own pace — every consumer sees every message (pub-sub semantics).
3. **Wait strategies** govern what a consumer does when no new message is available:
   - `BusySpin` — tight loop with `_mm_pause()` / `yield` (ARM), lowest latency, highest CPU usage
   - `Yielding` — spins briefly, then yields the CPU thread, moderate latency/CPU tradeoff
   - `Blocking` — spins briefly, then sleeps on `eventfd` until the producer sends a broadcast wakeup, near-zero CPU when idle

---

## Tech Stack

| Component | Technology | Why |
|---|---|---|
| Language | C++17 | Zero-cost abstractions, direct hardware control, `std::atomic`, `std::optional` |
| IPC | POSIX Shared Memory (`shm_open`, `mmap`) | Kernel-bypass after setup, zero-copy reads |
| Synchronization | `std::atomic` (lock-free) | No mutexes, no kernel transitions in hot path |
| Wait Notification | `eventfd` (Linux) | Lightweight broadcast wakeup, single fd for all consumers |
| Timing | `rdtsc` (x86 TSC) / `cntvct_el0` (ARM64) | Cycle-accurate latency measurement, ~10x faster than `clock_gettime()` |
| Build | CMake | Cross-platform build configuration |
| Containerization | Docker | Run on macOS / Windows (WSL2) / Linux without code changes |
| Platform | Linux (native or Docker) | Required for `shm_open`, `mmap`, `eventfd` |

---

## Project Structure

```
├── CMakeLists.txt                   # Build configuration (main + test targets)
├── Dockerfile                       # Ubuntu 24.04 build environment
├── docker-compose.yml               # Multi-service producer/consumer setup
├── run_demo.sh                      # One-command demo (Docker required)
├── LICENSE                          # MIT License
├── .gitignore
├── README.md
├── include/
│   ├── Queue.h                      # Lock-free SPMC ring buffer (templated)
│   ├── Message.h                    # Discriminated union message types
│   ├── Producer.h                   # Producer class (RAII shared memory + publish API)
│   ├── Consumer.h                   # Consumer base class with virtual onMessage() hook
│   ├── WaitStrategy.h               # WaitStrategy interface + BusySpin, Yielding, Blocking
│   ├── SharedMemoryManager.h        # RAII wrapper for shm_open / mmap / munmap / shm_unlink
│   ├── LatencyTracker.h             # Percentile latency collection (p50/p95/p99/max)
│   └── Utils.h                      # rdtsc / cntvct_el0, CPU frequency calibration
├── src/
│   ├── main.cpp                     # Entry point (producer / consumer / cleanup modes)
│   └── demo_single_process.cpp      # Single-process threaded demo (no shared memory needed)
├── tests/
│   └── test_queue.cpp               # 15 unit tests (correctness, concurrency, backpressure)
└── docs/
    └── DESIGN.md                    # Memory ordering rationale, algorithmic choices
```

---

## Key Concepts

### Ring Buffer (Circular Queue)
A fixed-size array (1024 slots, power-of-two) where the producer writes sequentially and wraps around using bitwise AND. No dynamic memory, no fragmentation, O(1) publish and consume.

### Lock-Free Coordination
The producer and consumers coordinate through atomic sequence numbers on each slot — no mutexes, no condition variables, no kernel involvement. Memory ordering (`acquire`/`release`) guarantees that data written by the producer is visible to consumers before the sequence number signals readiness.

### Pub-Sub Semantics
Every consumer maintains its own read position and independently receives every message. Consumers don't interfere with each other and can run at different speeds.

### Adaptive Wait Strategies
When no data is available, consumers choose how to wait:

| Strategy | How It Works | Latency | CPU Cost |
|---|---|---|---|
| `BusySpin` | Tight loop with `_mm_pause()` / `yield` (ARM) | ~100-500 ns | 100% of one core |
| `Yielding` | Spin N times, then `std::this_thread::yield()` | ~1-5 μs | Moderate |
| `Blocking` | Spin N times, then sleep on `eventfd`; producer broadcasts wakeup | ~5-15 μs | Near zero when idle |

### Cache-Line Alignment
Every shared data structure is aligned to 64-byte boundaries to prevent false sharing — where unrelated data on the same cache line causes expensive coherency traffic between CPU cores.

### Shared Memory IPC
The queue is constructed in a POSIX shared memory segment via `mmap`. Multiple processes attach to the same region. After initial setup, all communication happens in userspace with zero kernel involvement.

---

## Build & Run

### Option 1: Docker (macOS, Windows WSL2, or Linux)

This is the easiest way to run the project on any machine.

**Prerequisites:** [Docker Desktop](https://www.docker.com/products/docker-desktop/) installed and running.

**Run everything with one command:**
```bash
git clone https://github.com/sbidwaibing/LockFree-SPMC-Queue-Distribution-System.git
cd LockFree-SPMC-Queue-Distribution-System
chmod +x run_demo.sh
./run_demo.sh
```

This builds the Docker image, runs all 15 unit tests, then launches 1 producer + 3 consumers (one per wait strategy) and shows latency results.

**Run only tests:**
```bash
./run_demo.sh test
```

**Run only the demo (skip tests):**
```bash
./run_demo.sh demo
```

### Option 2: Native Linux (x86-64)

**Prerequisites:**
- Linux x86-64
- C++17 compiler (GCC 7+ or Clang 5+)
- CMake 3.10+

**Build:**
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

**Run tests:**
```bash
./build/test_queue
```

**Run the multi-process system (multiple terminals):**

Terminal 1 — Start the producer:
```bash
./build/lockfree_queue p
```

Terminals 2, 3, 4 — Start consumers (each in a separate terminal):
```bash
./build/lockfree_queue c spin     # BusySpin strategy
./build/lockfree_queue c yield    # Yielding strategy
./build/lockfree_queue c block    # Blocking strategy (eventfd)
```

Press **Enter** in the producer terminal to begin publishing 1,000,000 messages.

**Auto-start mode (no Enter required):**
```bash
./build/lockfree_queue p --auto       # waits 3s for consumers, then publishes
./build/lockfree_queue p --auto 5     # waits 5s for consumers
```

**Cleanup shared memory:**
```bash
./build/lockfree_queue x
```

### Option 3: Single-Process Demo (no shared memory or multiple terminals needed)

For environments like Google Colab or WSL where you can't open multiple terminals, a single-process threaded demo is included:

```bash
g++ -std=c++17 -O2 -I include -o demo src/demo_single_process.cpp -lpthread -lrt
./demo
```

This runs 1 producer thread + 3 consumer threads (one per wait strategy) in a single process and prints a side-by-side latency comparison.

---

## Performance

Latency measured via `rdtsc` (CPU timestamp counter) on a single machine, producer and consumers pinned to separate cores.

| Metric | BusySpin | Yielding | Blocking |
|---|---|---|---|
| Avg latency | ~0.1-0.5 μs | ~1-5 μs | ~5-15 μs |
| p99 latency | ~1 μs | ~10 μs | ~25 μs |
| CPU per consumer (idle) | 100% | ~5-20% | ~0% |
| CPU per consumer (active) | 100% | 100% | 100% |

> **Note:** Exact numbers depend on hardware, CPU frequency, core topology, and system load. These are representative ranges on a native x86-64 Linux system. Docker/ARM emulation will show higher latency due to timer counter differences.

---

## Design Decisions

See [docs/DESIGN.md](docs/DESIGN.md) for detailed rationale on:

- Why `memory_order_acquire` / `memory_order_release` (not `seq_cst`)
- Why single producer (and how to extend to multi-producer)
- Why discriminated unions over `std::variant` or inheritance
- Why `eventfd` over `futex` or condition variables for the blocking strategy
- Why lazy minimum-sequence caching instead of per-publish consumer scans

---

## License

This project is licensed under the MIT License — see [LICENSE](LICENSE) for details.

---

## References

- [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/) — the pattern this design is based on
- [LMAX Disruptor Whitepaper](https://lmax-exchange.github.io/disruptor/disruptor.html) — mechanical sympathy and lock-free design
- *Building Low Latency Applications with C++* by Sourav Ghosh — [Packt Publishing](https://www.packtpub.com/en-us/product/building-low-latency-applications-with-c-9781837634477)