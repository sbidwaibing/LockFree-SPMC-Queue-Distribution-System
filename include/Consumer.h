#pragma once

/**
 * @file Consumer.h
 * @brief Base consumer class with pluggable message handling and wait strategy.
 *
 * Encapsulates the consumer workflow:
 *   1. Attaches to existing shared memory (via SharedMemoryManager)
 *   2. Registers with the queue to get a consumer ID
 *   3. Runs a consume loop with the injected WaitStrategy
 *   4. Calls the virtual onMessage() hook for each received message
 *   5. Tracks latency statistics via LatencyTracker
 *   6. Unregisters and cleans up on destruction (RAII)
 *
 * Subclass Consumer and override onMessage() to implement custom
 * processing logic. For example:
 *   - LoggingConsumer:        prints every message
 *   - MatchingEngineConsumer: feeds messages into an order book
 *   - RiskConsumer:           monitors exposure limits
 *
 * This is the Strategy pattern (WaitStrategy) combined with the
 * Template Method pattern (onMessage hook).
 */

#include "Queue.h"
#include "Message.h"
#include "WaitStrategy.h"
#include "SharedMemoryManager.h"
#include "LatencyTracker.h"
#include "Utils.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace spmc {

/**
 * @brief Abstract base consumer with built-in wait strategy and latency tracking.
 *
 * @tparam T Message type (must match the producer's queue type).
 */
template <typename T>
class Consumer {
public:
    /**
     * @brief Construct a consumer, attaching to existing shared memory.
     *
     * @param shm_name       POSIX shared memory name (must match producer).
     * @param strategy        Wait strategy (ownership transferred).
     * @param cycles_per_ns   CPU frequency calibration for latency reporting.
     * @param max_messages    Expected message count (for LatencyTracker pre-allocation).
     * @param shutdown_flag   Reference to the producer's shutdown flag.
     */
    Consumer(const std::string& shm_name,
             std::unique_ptr<WaitStrategy> strategy,
             double cycles_per_ns,
             size_t max_messages,
             const std::atomic<bool>& shutdown_flag)
        : shm_manager_(shm_name, /*create=*/false)
        , strategy_(std::move(strategy))
        , cycles_per_ns_(cycles_per_ns)
        , tracker_(max_messages, cycles_per_ns)
        , shutdown_flag_(shutdown_flag) {

        queue_ = shm_manager_.get_queue();
        if (!queue_) {
            throw std::runtime_error("Failed to get queue from shared memory");
        }

        // Register with the queue. Start from sequence 1 (first published
        // message will have sequence 1).
        auto id = queue_->register_consumer(1);
        if (!id.has_value()) {
            throw std::runtime_error("Failed to register consumer: all slots full");
        }
        consumer_id_ = id.value();
    }

    virtual ~Consumer() {
        if (queue_ && consumer_id_ >= 0) {
            queue_->unregister_consumer(consumer_id_);
        }
    }

    // Non-copyable
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

    /**
     * @brief Run the consume loop until shutdown or max_messages reached.
     *
     * The loop:
     *   1. Try to consume a message
     *   2. If successful: record latency, call onMessage(), reset strategy
     *   3. If not: call strategy->wait_for_data()
     *   4. Check shutdown flag periodically
     *
     * @param max_messages Stop after consuming this many messages.
     *                     Pass 0 for unlimited (run until shutdown).
     */
    void run(size_t max_messages = 0) {
        size_t count = 0;

        while (true) {
            // Check shutdown (only when we're not getting data)
            if (max_messages > 0 && count >= max_messages) break;

            T msg;
            if (queue_->consume(consumer_id_, msg)) {
                // Record latency
                uint64_t now = rdtsc();
                tracker_.record(now - msg.timestamp_ns);

                // Dispatch to subclass handler
                onMessage(msg);
                count++;

                // Reset wait strategy to most aggressive phase
                strategy_->reset();
            } else {
                // No data available — use wait strategy
                strategy_->wait_for_data();

                // Check if producer signaled shutdown
                if (shutdown_flag_.load(std::memory_order_acquire)) {
                    // Drain any remaining messages before exiting
                    T remaining;
                    while (queue_->consume(consumer_id_, remaining)) {
                        uint64_t now = rdtsc();
                        tracker_.record(now - remaining.timestamp_ns);
                        onMessage(remaining);
                        count++;
                    }
                    break;
                }
            }
        }

        consumed_count_ = count;
    }

    /**
     * @brief Override this to handle each received message.
     *
     * Called in the hot path — keep it fast. Avoid heap allocation,
     * system calls, or blocking operations here.
     *
     * @param msg The consumed message.
     */
    virtual void onMessage(const T& msg) = 0;

    /** @brief Print latency statistics (p50/p95/p99/max). */
    void report_latency(const std::string& label = "") const {
        std::string full_label = label.empty()
            ? ("Consumer " + std::to_string(consumer_id_))
            : label;
        tracker_.report(full_label);
    }

    /** @brief Number of messages consumed so far. */
    size_t consumed_count() const { return consumed_count_; }

    /** @brief This consumer's ID in the queue. */
    int consumer_id() const { return consumer_id_; }

protected:
    LockFreePubSubQueue<T>*         queue_;
    int                             consumer_id_ = -1;

private:
    SharedMemoryManager<T>          shm_manager_;
    std::unique_ptr<WaitStrategy>   strategy_;
    double                          cycles_per_ns_;
    LatencyTracker                  tracker_;
    const std::atomic<bool>&        shutdown_flag_;
    size_t                          consumed_count_ = 0;
};

// ─── Concrete Consumer: LoggingConsumer ──────────────────────────────

/**
 * @brief A simple consumer that counts messages by type.
 *
 * Demonstrates subclassing Consumer with a concrete onMessage() handler.
 * In a real system this might write to a log file or forward to a
 * monitoring system.
 */
template <typename T>
class LoggingConsumer : public Consumer<T> {
public:
    using Consumer<T>::Consumer; // Inherit constructors

    void onMessage(const T& msg) override {
        switch (msg.type) {
            case MessageType::Add:    ++add_count_;    break;
            case MessageType::Trade:  ++trade_count_;  break;
            case MessageType::Delete: ++delete_count_; break;
        }
    }

    void print_summary() const {
        std::cout << "Consumer " << this->consumer_id() << " received:"
                  << " Add=" << add_count_
                  << " Trade=" << trade_count_
                  << " Delete=" << delete_count_
                  << " Total=" << this->consumed_count()
                  << std::endl;
    }

private:
    uint64_t add_count_    = 0;
    uint64_t trade_count_  = 0;
    uint64_t delete_count_ = 0;
};

} // namespace spmc
