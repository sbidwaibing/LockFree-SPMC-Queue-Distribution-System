/**
 * @file main.cpp
 * @brief Entry point for the lock-free SPMC queue demo.
 *
 * Modes:
 *   ./lockfree_queue p              — Run as producer
 *   ./lockfree_queue c [strategy]   — Run as consumer
 *   ./lockfree_queue x              — Clean up shared memory
 *
 * Strategy options for consumer:
 *   spin   (default) — BusySpinWaitStrategy
 *   yield            — YieldingWaitStrategy
 *   block            — BlockingWaitStrategy (eventfd)
 */

#include "Message.h"
#include "Queue.h"
#include "Producer.h"
#include "Consumer.h"
#include "WaitStrategy.h"
#include "SharedMemoryManager.h"
#include "Utils.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <sys/eventfd.h>

using namespace spmc;

static constexpr const char* kShmName       = "/lockfree_spmc_queue";
static constexpr int         kTotalMessages  = 1'000'000;

// ─── Producer Mode ───────────────────────────────────────────────────

void run_producer_mode(bool auto_start, int wait_seconds) {
    // Create a BlockingWaitStrategy for notification support.
    // Even if some consumers use BusySpin, the producer always
    // calls notify_all() — it's a no-op for non-blocking strategies.
    int efd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (efd == -1) {
        std::cerr << "Failed to create eventfd" << std::endl;
        return;
    }

    auto strategy = std::make_unique<BlockingWaitStrategy>(efd);
    Producer<Message> producer(kShmName, std::move(strategy));

    if (auto_start) {
        // Automated mode: wait for consumers to register, then start
        std::cout << "[Producer] Auto-start mode. Waiting "
                  << wait_seconds << "s for consumers..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
        std::cout << "[Producer] Starting publish..." << std::endl;
    } else {
        // Interactive mode: wait for user
        std::cout << "[Producer] Press Enter to start publishing "
                  << kTotalMessages << " messages..." << std::endl;
        std::cin.get();
    }

    // Cycle through message types: Add → Trade → Delete → Add → ...
    MessageType types[] = { MessageType::Add, MessageType::Trade, MessageType::Delete };
    int type_idx = 0;
    uint64_t add_count = 0, trade_count = 0, delete_count = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (unsigned int i = 1; i <= kTotalMessages; ++i) {
        bool published = false;
        while (!published) {
            switch (types[type_idx]) {
                case MessageType::Add:
                    published = producer.publish(
                        Message(AddOrder{1000 + i, 101.5 + i, 10 + i}));
                    if (published) ++add_count;
                    break;
                case MessageType::Trade:
                    published = producer.publish(
                        Message(Trade{2000 + i, 100.1 + i, 5 + i}));
                    if (published) ++trade_count;
                    break;
                case MessageType::Delete:
                    published = producer.publish(
                        Message(DeleteOrder{3000 + i}));
                    if (published) ++delete_count;
                    break;
            }
            type_idx = (type_idx + 1) % 3;

            if (!published) [[unlikely]] {
                SPMC_CPU_PAUSE();
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "[Producer] Published " << kTotalMessages
              << " messages in " << elapsed.count() << " seconds"
              << " | Add: " << add_count
              << ", Trade: " << trade_count
              << ", Delete: " << delete_count
              << std::endl;

    // Signal consumers to shut down
    producer.shutdown();
    std::cout << "[Producer] Shutdown signal sent." << std::endl;
}

// ─── Consumer Mode ───────────────────────────────────────────────────

void run_consumer_mode(const std::string& strategy_name) {
    // Calibrate CPU frequency for latency conversion
    std::cout << "[Consumer] Calibrating CPU frequency..." << std::endl;
    double cycles_per_ns = calibrate_cycles_per_ns();
    std::cout << "[Consumer] Calibrated: " << cycles_per_ns
              << " cycles/ns" << std::endl;

    // Create the appropriate wait strategy
    std::unique_ptr<WaitStrategy> strategy;

    if (strategy_name == "yield") {
        strategy = std::make_unique<YieldingWaitStrategy>(1000);
        std::cout << "[Consumer] Using YieldingWaitStrategy (spin_limit=1000)"
                  << std::endl;
    } else if (strategy_name == "block") {
        int efd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
        if (efd == -1) {
            std::cerr << "Failed to create eventfd" << std::endl;
            return;
        }
        strategy = std::make_unique<BlockingWaitStrategy>(efd, 1000, 100);
        std::cout << "[Consumer] Using BlockingWaitStrategy (eventfd)"
                  << std::endl;
    } else {
        strategy = std::make_unique<BusySpinWaitStrategy>();
        std::cout << "[Consumer] Using BusySpinWaitStrategy" << std::endl;
    }

    // We need a shutdown flag. In a real multi-process setup this would
    // live in shared memory. For this demo we use a local flag and rely
    // on message count to stop.
    static std::atomic<bool> shutdown_flag{false};

    LoggingConsumer<Message> consumer(
        kShmName,
        std::move(strategy),
        cycles_per_ns,
        kTotalMessages,
        shutdown_flag
    );

    std::cout << "[Consumer " << consumer.consumer_id()
              << "] Registered. Waiting for messages..." << std::endl;

    // Run until we've consumed all expected messages
    consumer.run(kTotalMessages);

    // Report results
    consumer.print_summary();
    consumer.report_latency();
}

// ─── Cleanup Mode ────────────────────────────────────────────────────

void run_cleanup() {
    SharedMemoryManager<Message>::unlink(kShmName);
    std::cout << "[Cleanup] Shared memory '" << kShmName
              << "' unlinked." << std::endl;
}

// ─── Main ────────────────────────────────────────────────────────────

void print_usage(const char* prog) {
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  " << prog << " p [--auto [wait_secs]]  — Run as producer" << std::endl;
    std::cerr << "  " << prog << " c [strategy]            — Run as consumer" << std::endl;
    std::cerr << "  " << prog << " x                       — Clean up shared memory" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Producer options:" << std::endl;
    std::cerr << "  --auto [N]    Skip Enter prompt, wait N seconds (default 3) for consumers" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Strategy options:" << std::endl;
    std::cerr << "  spin   (default)  — BusySpinWaitStrategy" << std::endl;
    std::cerr << "  yield             — YieldingWaitStrategy" << std::endl;
    std::cerr << "  block             — BlockingWaitStrategy (eventfd)" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string mode(argv[1]);

    if (mode == "p") {
        bool auto_start = false;
        int wait_seconds = 3;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--auto") {
                auto_start = true;
                if (i + 1 < argc) {
                    try { wait_seconds = std::stoi(argv[i + 1]); ++i; }
                    catch (...) { /* not a number, use default */ }
                }
            }
        }
        run_producer_mode(auto_start, wait_seconds);
    } else if (mode == "c") {
        std::string strategy = (argc >= 3) ? argv[2] : "spin";
        run_consumer_mode(strategy);
    } else if (mode == "x") {
        run_cleanup();
    } else {
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
