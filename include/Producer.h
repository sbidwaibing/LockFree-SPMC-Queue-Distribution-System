#pragma once

/**
 * @file Producer.h
 * @brief RAII producer that owns the shared memory and publishes messages.
 *
 * Encapsulates the producer workflow:
 *   1. Creates/owns the shared memory segment (via SharedMemoryManager)
 *   2. Publishes messages to the queue
 *   3. Notifies blocking consumers via eventfd after each publish
 *   4. Signals shutdown to all consumers via an atomic flag
 *
 * The producer is the owner of the shared memory lifecycle. When the
 * Producer is destroyed, the shared memory is unlinked.
 */

#include "Queue.h"
#include "WaitStrategy.h"
#include "SharedMemoryManager.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace spmc {

/**
 * @brief Manages publishing messages to the lock-free queue.
 *
 * @tparam T Message type (must be trivially copyable).
 */
template <typename T>
class Producer {
public:
    /**
     * @brief Construct a producer, creating the shared memory segment.
     *
     * @param shm_name  POSIX shared memory name (e.g., "/my_queue").
     * @param strategy  Wait strategy used to notify consumers.
     *                  Typically a BusySpin (no-op notify) or Blocking
     *                  (eventfd notify). Ownership is transferred.
     */
    Producer(const std::string& shm_name,
             std::unique_ptr<WaitStrategy> strategy)
        : shm_manager_(shm_name, /*create=*/true)
        , strategy_(std::move(strategy)) {

        queue_ = shm_manager_.get_queue();
        if (!queue_) {
            throw std::runtime_error("Failed to get queue from shared memory");
        }
    }

    // Non-copyable, non-movable (owns system resources)
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    /**
     * @brief Publish a message to the queue.
     *
     * After a successful publish, calls notify_all() on the wait strategy
     * to wake any blocking consumers.
     *
     * @param item The message to publish.
     * @return true if published, false if ring is full (backpressure).
     */
    bool publish(const T& item) {
        bool ok = queue_->publish(item);
        if (ok) {
            strategy_->notify_all();
        }
        return ok;
    }

    /**
     * @brief Signal all consumers that no more messages will be published.
     *
     * Sets the shutdown flag and sends a final notification to wake any
     * sleeping consumers so they can observe the flag and exit.
     */
    void shutdown() {
        shutdown_flag_.store(true, std::memory_order_release);
        strategy_->notify_all();
    }

    /** @brief Check if shutdown has been signaled. */
    bool is_shutdown() const {
        return shutdown_flag_.load(std::memory_order_acquire);
    }

    /** @brief Get the current producer sequence number. */
    uint64_t current_sequence() const { return queue_->current_sequence(); }

    /** @brief Access the underlying queue (for consumer registration). */
    LockFreePubSubQueue<T>* get_queue() { return queue_; }

    /** @brief Get the eventfd (needed by BlockingWaitStrategy consumers). */
    int get_event_fd() const { return shm_manager_.get_event_fd(); }

    /** @brief Access the shutdown flag (consumers check this). */
    const std::atomic<bool>& get_shutdown_flag() const { return shutdown_flag_; }

private:
    SharedMemoryManager<T>          shm_manager_;
    LockFreePubSubQueue<T>*         queue_;
    std::unique_ptr<WaitStrategy>   strategy_;
    std::atomic<bool>               shutdown_flag_{false};
};

} // namespace spmc
