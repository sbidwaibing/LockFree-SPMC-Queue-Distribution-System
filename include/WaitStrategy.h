#pragma once

/**
 * @file WaitStrategy.h
 * @brief Adaptive wait strategies for consumers.
 *
 * Defines a pluggable interface that governs what a consumer does when
 * no new message is available in the ring buffer. Three implementations
 * provide different tradeoffs between latency and CPU usage:
 *
 *   BusySpinWaitStrategy  — lowest latency, highest CPU burn
 *   YieldingWaitStrategy   — moderate latency, moderate CPU
 *   BlockingWaitStrategy   — higher latency, near-zero CPU when idle
 *
 * This follows the Strategy design pattern: consumers are injected with
 * a WaitStrategy at construction time and can be swapped without changing
 * the consumer's core logic.
 *
 * The BlockingWaitStrategy uses Linux eventfd for broadcast notifications.
 * When no data is available after a spin phase, the consumer sleeps on the
 * eventfd. The producer writes to the same eventfd after publishing,
 * waking all sleeping consumers simultaneously.
 *
 * Design inspired by the LMAX Disruptor's WaitStrategy hierarchy:
 *   - BusySpinWaitStrategy  ≈ Disruptor's BusySpinWaitStrategy
 *   - YieldingWaitStrategy   ≈ Disruptor's YieldingWaitStrategy
 *   - BlockingWaitStrategy   ≈ Disruptor's BlockingWaitStrategy
 */

#include <cstdint>
#include <thread>

// Platform-specific CPU pause/yield hint
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <x86intrin.h>
    #define SPMC_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    // ARM yield hint — tells the core it's in a spin-wait
    #define SPMC_CPU_PAUSE() asm volatile("yield" ::: "memory")
#else
    // Portable fallback — no-op, compiler barrier only
    #define SPMC_CPU_PAUSE() asm volatile("" ::: "memory")
#endif

// Linux-specific headers for eventfd + poll
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>

namespace spmc {

// ─── WaitStrategy Interface ──────────────────────────────────────────

/**
 * @brief Abstract interface for consumer wait strategies.
 *
 * A consumer calls wait_for_data() in its poll loop when consume()
 * returns false (no data available). The strategy decides how to wait.
 *
 * The producer calls notify_all() after publishing a message. Only
 * BlockingWaitStrategy uses this to wake sleeping consumers; the other
 * strategies ignore it (no-op).
 */
class WaitStrategy {
public:
    virtual ~WaitStrategy() = default;

    /**
     * @brief Called by a consumer when no message is available.
     *
     * The implementation decides how to wait: spin, yield, or sleep.
     * Must be safe to call from multiple consumer threads concurrently.
     */
    virtual void wait_for_data() = 0;

    /**
     * @brief Called by the producer after publishing a message.
     *
     * Used by BlockingWaitStrategy to wake sleeping consumers via eventfd.
     * BusySpin and Yielding strategies implement this as a no-op since
     * their consumers are already actively polling.
     */
    virtual void notify_all() = 0;

    /**
     * @brief Reset internal state (e.g., spin counters).
     *
     * Called by the consumer after a successful consume() to reset the
     * strategy back to its most aggressive phase. For example, the
     * YieldingWaitStrategy resets its spin counter back to zero so the
     * next idle period starts with spinning before yielding.
     */
    virtual void reset() = 0;
};

// ─── BusySpinWaitStrategy ────────────────────────────────────────────

/**
 * @brief Spin in a tight loop using _mm_pause().
 *
 * The _mm_pause() intrinsic inserts a PAUSE instruction which:
 *   - Hints the CPU that this is a spin-wait loop
 *   - Reduces power consumption slightly
 *   - Avoids memory order violations on out-of-order CPUs
 *   - Prevents the spin loop from starving the memory bus
 *
 * Use when: absolute minimum latency is required and dedicating an
 * entire CPU core per consumer is acceptable (e.g., HFT systems).
 *
 * Latency:   ~100-500 ns (limited only by cache coherency propagation)
 * CPU usage: 100% of one core, always
 */
class BusySpinWaitStrategy : public WaitStrategy {
public:
    void wait_for_data() override {
        SPMC_CPU_PAUSE();
    }

    void notify_all() override {
        // No-op: consumers are already spinning
    }

    void reset() override {
        // No state to reset
    }
};

// ─── YieldingWaitStrategy ────────────────────────────────────────────

/**
 * @brief Spin for N iterations, then yield the thread.
 *
 * Phase 1 (spin): Execute _mm_pause() for spin_limit_ iterations.
 *                 This handles the common case where the producer is
 *                 just a few nanoseconds behind — data arrives before
 *                 we finish spinning.
 *
 * Phase 2 (yield): Call std::this_thread::yield() which invokes the
 *                  OS scheduler's sched_yield(). This lets other threads
 *                  run but keeps the consumer runnable — it will be
 *                  rescheduled quickly.
 *
 * On a successful consume(), reset() is called to return to Phase 1.
 *
 * Use when: low latency is important but not at the cost of burning
 * an entire core. Good for systems with more consumers than cores.
 *
 * Latency:   ~1-5 μs (depends on scheduler responsiveness)
 * CPU usage: moderate — high during spin, drops during yield
 */
class YieldingWaitStrategy : public WaitStrategy {
public:
    /**
     * @param spin_limit Number of _mm_pause() iterations before yielding.
     *                   Higher = lower latency but more CPU burn.
     *                   Default 1000 is a reasonable starting point.
     */
    explicit YieldingWaitStrategy(uint32_t spin_limit = 1000)
        : spin_limit_(spin_limit), spin_count_(0) {}

    void wait_for_data() override {
        if (spin_count_ < spin_limit_) {
            SPMC_CPU_PAUSE();
            ++spin_count_;
        } else {
            std::this_thread::yield();
        }
    }

    void notify_all() override {
        // No-op: consumers are spinning or will be rescheduled quickly
    }

    void reset() override {
        spin_count_ = 0;
    }

private:
    uint32_t spin_limit_;
    uint32_t spin_count_;
};

// ─── BlockingWaitStrategy ────────────────────────────────────────────

/**
 * @brief Spin briefly, then sleep on eventfd until the producer wakes us.
 *
 * Phase 1 (spin):  Execute _mm_pause() for spin_limit_ iterations.
 *                  Handles the fast path — data usually arrives here.
 *
 * Phase 2 (block): Call poll() on the eventfd file descriptor, which
 *                  puts the thread to sleep in the kernel. The thread
 *                  consumes zero CPU until the producer calls notify_all()
 *                  which writes to the eventfd, waking ALL blocked consumers.
 *
 * On a successful consume(), reset() returns to Phase 1.
 *
 * The eventfd is created in semaphore mode (EFD_SEMAPHORE) so that
 * each consumer's read() decrements the counter independently, and
 * EFD_NONBLOCK so that reads don't block if the counter is already zero.
 *
 * Use when: CPU efficiency matters more than ultra-low latency. Ideal
 * for monitoring/logging consumers that can tolerate microsecond-level
 * wakeup delays, or when running many consumers on limited cores.
 *
 * Latency:   ~5-15 μs (kernel wakeup + scheduler delay)
 * CPU usage: near zero when idle, 100% when actively consuming
 *
 * @note The eventfd file descriptor must be shared between the producer
 *       and all consumers that use this strategy. In a shared memory
 *       setup, the fd is obtained via the SharedMemoryManager and passed
 *       to both the Producer and Consumer instances.
 */
class BlockingWaitStrategy : public WaitStrategy {
public:
    /**
     * @param event_fd     A Linux eventfd file descriptor shared with the
     *                     producer. Created by SharedMemoryManager.
     * @param spin_limit   Spin iterations before falling back to sleep.
     * @param poll_timeout_ms  Maximum time to sleep in poll() before
     *                         waking up to recheck. Prevents indefinite
     *                         hangs if a notification is missed. -1 for
     *                         infinite wait.
     */
    explicit BlockingWaitStrategy(int event_fd,
                                  uint32_t spin_limit = 1000,
                                  int poll_timeout_ms = 100)
        : event_fd_(event_fd)
        , spin_limit_(spin_limit)
        , poll_timeout_ms_(poll_timeout_ms)
        , spin_count_(0) {}

    void wait_for_data() override {
        if (spin_count_ < spin_limit_) {
            // Phase 1: Spin — handle the fast path
            SPMC_CPU_PAUSE();
            ++spin_count_;
        } else {
            // Phase 2: Block — sleep on eventfd until producer notifies
            struct pollfd pfd;
            pfd.fd = event_fd_;
            pfd.events = POLLIN;

            int ret = poll(&pfd, 1, poll_timeout_ms_);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                // Drain the eventfd counter so it can be signaled again.
                // Using read() in non-blocking mode — if another consumer
                // already drained it, this harmlessly returns EAGAIN.
                uint64_t val;
                [[maybe_unused]] ssize_t r = read(event_fd_, &val, sizeof(val));
            }
            // Whether poll timed out or was signaled, we return to the
            // consumer loop which will call consume() to check for data.
        }
    }

    /**
     * @brief Wake all consumers sleeping on the eventfd.
     *
     * Called by the producer after publishing a message. The write()
     * increments the eventfd counter, causing poll() to return POLLIN
     * in all consumers blocked on this fd.
     *
     * The value 1 is sufficient — multiple writes accumulate, and each
     * consumer drains independently.
     */
    void notify_all() override {
        uint64_t val = 1;
        [[maybe_unused]] ssize_t r = write(event_fd_, &val, sizeof(val));
    }

    void reset() override {
        spin_count_ = 0;
    }

private:
    int      event_fd_;
    uint32_t spin_limit_;
    int      poll_timeout_ms_;
    uint32_t spin_count_;
};

} // namespace spmc
