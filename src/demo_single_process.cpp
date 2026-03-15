/**
 * @file demo_single_process.cpp
 * @brief Single-process demo for environments without multiple terminals.
 *
 * Runs the producer and 3 consumers (one per wait strategy) as threads
 * within a single process. This works on Google Colab, WSL, or any
 * Linux environment where you can't open multiple terminals.
 *
 * The queue works identically for threads and processes — the only
 * difference is that this demo doesn't use shared memory (not needed
 * when everything is in one process).
 *
 * Build:
 *   g++ -std=c++17 -O2 -march=native -I include -o demo demo_single_process.cpp -lpthread
 *
 * Run:
 *   ./demo
 */

#include "Queue.h"
#include "Message.h"
#include "WaitStrategy.h"
#include "LatencyTracker.h"
#include "Utils.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <memory>
#include <string>

using namespace spmc;

static constexpr int kTotalMessages = 1'000'000;

// ─── Consumer Thread Function ────────────────────────────────────────

struct ConsumerResult {
    std::string strategy_name;
    uint64_t add_count    = 0;
    uint64_t trade_count  = 0;
    uint64_t delete_count = 0;
    uint64_t total        = 0;
};

void consumer_thread(LockFreePubSubQueue<Message>* queue,
                     int consumer_id,
                     std::unique_ptr<WaitStrategy> strategy,
                     double cycles_per_ns,
                     const std::string& strategy_name,
                     const std::atomic<bool>& shutdown_flag,
                     ConsumerResult& result,
                     LatencyTracker& tracker) {

    result.strategy_name = strategy_name;

    while (result.total < kTotalMessages) {
        Message msg;
        if (queue->consume(consumer_id, msg)) {
            // Record latency
            uint64_t now = rdtsc();
            tracker.record(now - msg.timestamp_ns);

            // Count by type
            switch (msg.type) {
                case MessageType::Add:    ++result.add_count;    break;
                case MessageType::Trade:  ++result.trade_count;  break;
                case MessageType::Delete: ++result.delete_count; break;
            }
            result.total++;

            // Reset strategy to aggressive phase after success
            strategy->reset();
        } else {
            strategy->wait_for_data();

            // Check shutdown
            if (shutdown_flag.load(std::memory_order_acquire)) {
                // Drain remaining
                Message remaining;
                while (queue->consume(consumer_id, remaining)) {
                    uint64_t now = rdtsc();
                    tracker.record(now - remaining.timestamp_ns);
                    result.total++;
                }
                break;
            }
        }
    }
}

// ─── Main ────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Lock-Free SPMC Queue Demo (Single Process) ===" << std::endl;
    std::cout << "Publishing " << kTotalMessages << " messages to 3 consumers" << std::endl;
    std::cout << std::endl;

    // Calibrate CPU frequency
    std::cout << "Calibrating CPU frequency..." << std::endl;
    double cycles_per_ns = calibrate_cycles_per_ns();
    std::cout << "CPU frequency: " << cycles_per_ns << " cycles/ns ("
              << (cycles_per_ns * 1000.0) << " MHz)" << std::endl;
    std::cout << std::endl;

    // Create the queue (stack-allocated, no shared memory needed)
    LockFreePubSubQueue<Message> queue;
    std::atomic<bool> shutdown_flag{false};

    // Register 3 consumers with different wait strategies
    auto c0 = queue.register_consumer(1);
    auto c1 = queue.register_consumer(1);
    auto c2 = queue.register_consumer(1);

    if (!c0 || !c1 || !c2) {
        std::cerr << "Failed to register consumers" << std::endl;
        return 1;
    }

    // Create eventfd for the blocking strategy
    int efd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (efd == -1) {
        std::cerr << "Failed to create eventfd" << std::endl;
        return 1;
    }

    // Prepare results and trackers
    ConsumerResult results[3];
    LatencyTracker tracker0(kTotalMessages, cycles_per_ns);
    LatencyTracker tracker1(kTotalMessages, cycles_per_ns);
    LatencyTracker tracker2(kTotalMessages, cycles_per_ns);

    // Launch consumer threads
    std::cout << "Starting consumers..." << std::endl;

    std::thread t0(consumer_thread, &queue, c0.value(),
                   std::make_unique<BusySpinWaitStrategy>(),
                   cycles_per_ns, "BusySpin", std::ref(shutdown_flag),
                   std::ref(results[0]), std::ref(tracker0));

    std::thread t1(consumer_thread, &queue, c1.value(),
                   std::make_unique<YieldingWaitStrategy>(1000),
                   cycles_per_ns, "Yielding", std::ref(shutdown_flag),
                   std::ref(results[1]), std::ref(tracker1));

    std::thread t2(consumer_thread, &queue, c2.value(),
                   std::make_unique<BlockingWaitStrategy>(efd, 1000, 100),
                   cycles_per_ns, "Blocking", std::ref(shutdown_flag),
                   std::ref(results[2]), std::ref(tracker2));

    // Small delay to let consumers start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── Produce ──────────────────────────────────────────────────────

    std::cout << "Publishing " << kTotalMessages << " messages..." << std::endl;

    MessageType types[] = { MessageType::Add, MessageType::Trade, MessageType::Delete };
    int type_idx = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (unsigned int i = 1; i <= kTotalMessages; ++i) {
        bool published = false;
        while (!published) {
            switch (types[type_idx]) {
                case MessageType::Add:
                    published = queue.publish(
                        Message(AddOrder{1000 + i, 101.5 + i, 10 + i}));
                    break;
                case MessageType::Trade:
                    published = queue.publish(
                        Message(Trade{2000 + i, 100.1 + i, 5 + i}));
                    break;
                case MessageType::Delete:
                    published = queue.publish(
                        Message(DeleteOrder{3000 + i}));
                    break;
            }
            type_idx = (type_idx + 1) % 3;

            if (!published) [[unlikely]] {
                _mm_pause();
            }
        }

        // Notify blocking consumers
        if (efd != -1) {
            uint64_t val = 1;
            [[maybe_unused]] ssize_t r = write(efd, &val, sizeof(val));
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "Published " << kTotalMessages << " messages in "
              << elapsed.count() << " seconds ("
              << static_cast<int>(kTotalMessages / elapsed.count())
              << " msgs/sec)" << std::endl;
    std::cout << std::endl;

    // Signal shutdown and wait for consumers
    shutdown_flag.store(true, std::memory_order_release);
    if (efd != -1) {
        uint64_t val = 1;
        [[maybe_unused]] ssize_t r = write(efd, &val, sizeof(val));
    }

    t0.join();
    t1.join();
    t2.join();

    // ── Report Results ───────────────────────────────────────────────

    std::cout << "=== Consumer Results ===" << std::endl;
    for (int i = 0; i < 3; ++i) {
        std::cout << "[" << results[i].strategy_name << "] "
                  << "Total: " << results[i].total
                  << " | Add: " << results[i].add_count
                  << " | Trade: " << results[i].trade_count
                  << " | Delete: " << results[i].delete_count
                  << std::endl;
    }
    std::cout << std::endl;

    std::cout << "=== Latency Comparison ===" << std::endl;
    tracker0.report("BusySpin ");
    std::cout << std::endl;
    tracker1.report("Yielding ");
    std::cout << std::endl;
    tracker2.report("Blocking ");

    // Cleanup
    queue.unregister_consumer(c0.value());
    queue.unregister_consumer(c1.value());
    queue.unregister_consumer(c2.value());
    if (efd != -1) close(efd);

    return 0;
}
