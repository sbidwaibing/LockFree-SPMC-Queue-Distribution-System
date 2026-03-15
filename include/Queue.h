#pragma once

/**
 * @file Queue.h
 * @brief Lock-free single-producer multi-consumer (SPMC) ring buffer.
 *
 * This is the core data structure of the system. A fixed-size circular
 * array where one producer writes messages and up to MaxConsumers
 * consumers independently read every message (pub-sub semantics).
 *
 * Coordination is entirely lock-free using atomic sequence numbers:
 *   - Producer: writes data to slot, then store(release) the sequence
 *   - Consumer: load(acquire) the sequence, then reads data
 *   - This acquire-release pair establishes happens-before ordering
 *
 * Design inspired by the LMAX Disruptor pattern.
 *
 * @note This class is designed to live in shared memory (via mmap).
 *       All members are trivially copyable. No pointers, no heap
 *       allocation, no virtual functions.
 */

#include <atomic>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace spmc {

// ─── Configuration Constants ─────────────────────────────────────────
// Ring size must be a power of two for bitwise index computation.
// seq & kRingMask is equivalent to seq % kRingSize but compiles to
// a single AND instruction — no division, no branch.

constexpr size_t kRingSize     = 1024;
constexpr size_t kRingMask     = kRingSize - 1;
constexpr int    MaxConsumers   = 64;
constexpr size_t kCacheLineSize = 64;

// ─── Slot ────────────────────────────────────────────────────────────

/**
 * @brief A single ring buffer entry.
 *
 * Each slot holds one message and an atomic sequence number that acts
 * as a version counter. The sequence is used to coordinate between the
 * producer and consumers without locks:
 *   - Producer sets sequence = (published_seq + 1) after writing data
 *   - Consumer checks if sequence > expected_seq to know data is ready
 *
 * Aligned to cache line boundary (64 bytes) to prevent false sharing
 * between adjacent slots.
 *
 * @tparam T Message type (must be trivially copyable).
 */
template <typename T>
struct alignas(kCacheLineSize) Slot {
    std::atomic<uint64_t> sequence{0};
    T data;
};

// ─── Sequence ────────────────────────────────────────────────────────

/**
 * @brief Cache-line-aligned atomic sequence counter.
 *
 * Wraps a single std::atomic<uint64_t> with acquire/release semantics.
 * Each instance occupies exactly one cache line (64 bytes) to prevent
 * false sharing when multiple consumers' sequences are stored adjacently.
 */
class alignas(kCacheLineSize) Sequence {
public:
    explicit Sequence(uint64_t initial = 0) : value_(initial) {}

    /** @brief Load with acquire semantics (pairs with producer's release). */
    uint64_t get() const { return value_.load(std::memory_order_acquire); }

    /** @brief Store with release semantics (makes prior writes visible). */
    void set(uint64_t v) { value_.store(v, std::memory_order_release); }

private:
    std::atomic<uint64_t> value_;
};

// ─── ConsumerSlot ────────────────────────────────────────────────────

/**
 * @brief Per-consumer registration state.
 *
 * Tracks whether a consumer is active and its current read position.
 * Cache-line aligned so that one consumer's state changes don't
 * invalidate another consumer's cache line.
 */
struct alignas(kCacheLineSize) ConsumerSlot {
    std::atomic<bool> active{false};
    Sequence sequence{0};
};

// ─── LockFreePubSubQueue ────────────────────────────────────────────

/**
 * @brief The lock-free SPMC pub-sub queue.
 *
 * Thread/process safety guarantees:
 *   - publish()            : call from ONE producer only (single-producer)
 *   - consume()            : call from any registered consumer (multi-consumer)
 *   - register_consumer()  : thread-safe (uses CAS)
 *   - unregister_consumer(): thread-safe (atomic store)
 *
 * Memory layout is fixed and contains no pointers, making it safe to
 * place entirely within a POSIX shared memory segment.
 *
 * @tparam T Message type. Must be trivially copyable.
 */
template <typename T>
class LockFreePubSubQueue {
public:
    LockFreePubSubQueue() = default;

    // ── Consumer Management ──────────────────────────────────────────

    /**
     * @brief Atomically register a new consumer.
     *
     * Scans the consumer slot array for an inactive slot and claims it
     * using compare_exchange_strong (CAS). This is lock-free: if two
     * consumers race for the same slot, exactly one wins, the other
     * moves to the next slot.
     *
     * @param start_sequence The sequence number this consumer will begin
     *                       reading from. Typically set to the current
     *                       producer position (next_) so the consumer
     *                       starts from "now" rather than the beginning.
     *
     * @return Consumer ID (0-63) on success, std::nullopt if all slots full.
     */
    std::optional<int> register_consumer(uint64_t start_sequence) {
        for (int i = 0; i < MaxConsumers; ++i) {
            bool expected = false;
            if (consumers_[i].active.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                consumers_[i].sequence.set(start_sequence);
                return i;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Unregister a consumer by ID.
     *
     * Marks the consumer slot as inactive. The slot can be reused by
     * a future register_consumer() call. Does not block any other
     * consumer or the producer.
     *
     * @param id Consumer ID returned by register_consumer().
     */
    void unregister_consumer(int id) {
        if (id >= 0 && id < MaxConsumers) {
            consumers_[id].active.store(false, std::memory_order_release);
        }
    }

    // ── Producer ─────────────────────────────────────────────────────

    /**
     * @brief Publish a message to the ring buffer.
     *
     * Steps:
     *   1. Compute next sequence number.
     *   2. Fast-path: check cached min_consumer_seq_ to see if ring is full.
     *   3. Slow-path (unlikely): rescan all consumers for true minimum.
     *   4. Write message data into the target slot.
     *   5. Store (release) the slot's sequence to signal readiness.
     *
     * @param item The message to publish.
     * @return true if published, false if ring is full (backpressure).
     *
     * @note Single-producer only. Do NOT call from multiple threads/processes.
     */
    bool publish(const T& item) {
        uint64_t next_seq = next_ + 1;

        // Fast path: check cached minimum to avoid scanning consumers.
        // The [[unlikely]] hint tells the compiler this branch is rarely
        // taken, improving branch prediction for the common case.
        if ((next_seq - min_consumer_seq_) >= kRingSize) [[unlikely]] {

            // Slow path: recalculate the true minimum sequence across
            // all active consumers. This is O(MaxConsumers) but only
            // triggers when the ring appears full.
            uint64_t min_seq = std::numeric_limits<uint64_t>::max();
            for (int i = 0; i < MaxConsumers; ++i) {
                if (consumers_[i].active.load(std::memory_order_acquire)) {
                    min_seq = std::min(min_seq, consumers_[i].sequence.get());
                }
            }
            min_consumer_seq_ = min_seq;

            if ((next_seq - min_seq) >= kRingSize) {
                return false; // Ring truly full — backpressure
            }
        }

        // Claim the sequence number
        next_ = next_seq;

        // Write message to the target slot
        Slot<T>& slot = ring_[next_seq & kRingMask];
        slot.data = item;

        // Signal readiness to consumers. The release store ensures that
        // the data write (slot.data = item) is visible to any consumer
        // that reads this sequence with an acquire load.
        slot.sequence.store(next_seq + 1, std::memory_order_release);
        return true;
    }

    // ── Consumer ─────────────────────────────────────────────────────

    /**
     * @brief Attempt to consume the next message for a given consumer.
     *
     * Steps:
     *   1. Validate consumer ID and active status.
     *   2. Load the consumer's expected sequence number.
     *   3. Check if the corresponding slot has been published.
     *   4. If ready: copy message out, advance consumer's sequence.
     *
     * @param consumer_id ID returned by register_consumer().
     * @param[out] out    Message will be written here if available.
     * @return true if a message was consumed, false if nothing available.
     *
     * @note Each consumer operates independently. Multiple consumers can
     *       call consume() concurrently without coordination.
     */
    bool consume(int consumer_id, T& out) {
        if (consumer_id < 0 || consumer_id >= MaxConsumers) [[unlikely]]
            return false;
        if (!consumers_[consumer_id].active.load(std::memory_order_acquire)) [[unlikely]]
            return false;

        Sequence& consumer_seq = consumers_[consumer_id].sequence;

        // What sequence number does this consumer expect next?
        uint64_t seq = consumer_seq.get();
        Slot<T>& slot = ring_[seq & kRingMask];

        // Has the producer written this slot yet?
        // The acquire load pairs with the producer's release store,
        // guaranteeing that slot.data is fully visible if this check passes.
        if (slot.sequence.load(std::memory_order_acquire) <= seq) {
            return false; // Not ready yet
        }

        // Copy the message out
        out = slot.data;

        // Advance this consumer's read position
        consumer_seq.set(seq + 1);
        return true;
    }

    // ── Accessors ────────────────────────────────────────────────────

    /** @brief Current producer sequence (next message will be next_ + 1). */
    uint64_t current_sequence() const { return next_; }

private:
    alignas(kCacheLineSize) std::array<Slot<T>, kRingSize>          ring_{};
    alignas(kCacheLineSize) std::array<ConsumerSlot, MaxConsumers>  consumers_{};

    // Producer-only state. Not atomic because single-producer guarantee.
    // Would need to be atomic for multi-producer extension.
    uint64_t next_{0};
    uint64_t min_consumer_seq_{0};
};

} // namespace spmc
