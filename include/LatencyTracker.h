#pragma once

/**
 * @file LatencyTracker.h
 * @brief Percentile-based latency collector for consumer benchmarking.
 *
 * The original implementation only reported average latency, which hides
 * tail spikes. In trading systems, p99 latency is what actually matters —
 * a system that averages 0.5μs but spikes to 50μs at p99 is unreliable.
 *
 * This tracker collects raw cycle-count samples and computes percentiles
 * on demand. It pre-allocates a fixed-size buffer to avoid heap allocation
 * during measurement (which would distort the results).
 *
 * Usage:
 *   LatencyTracker tracker(1'000'000, cycles_per_ns);
 *   // In hot loop:
 *   tracker.record(rdtsc() - msg.timestamp_ns);
 *   // After:
 *   tracker.report();
 */

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <vector>

namespace spmc {

/**
 * @brief Collects latency samples and reports percentile statistics.
 *
 * Samples are stored as raw CPU cycle counts. Conversion to microseconds
 * happens only at report time using the calibrated cycles_per_ns factor.
 */
class LatencyTracker {
public:
    /**
     * @param capacity      Maximum number of samples to store.
     *                      Pre-allocated at construction to avoid
     *                      runtime allocation.
     * @param cycles_per_ns CPU frequency calibration factor from
     *                      calibrate_cycles_per_ns().
     */
    explicit LatencyTracker(size_t capacity, double cycles_per_ns)
        : cycles_per_ns_(cycles_per_ns) {
        samples_.reserve(capacity);
    }

    /**
     * @brief Record a latency sample (in CPU cycles).
     *
     * Called in the consumer hot path. Only a vector push_back —
     * no sorting, no computation.
     *
     * @param cycles The delta between rdtsc() now and the message's
     *               timestamp_ns field.
     */
    void record(uint64_t cycles) {
        if (samples_.size() < samples_.capacity()) {
            samples_.push_back(cycles);
        }
    }

    /**
     * @brief Compute and print latency statistics.
     *
     * Sorts the samples (O(n log n)) and extracts percentiles.
     * Call after the measurement run is complete, not in the hot path.
     *
     * @param label Optional label prefix (e.g., "Consumer 0").
     */
    void report(const std::string& label = "Latency") const {
        if (samples_.empty()) {
            std::cout << label << ": no samples collected" << std::endl;
            return;
        }

        // Sort a copy so we don't mutate the original
        std::vector<uint64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());

        size_t n = sorted.size();

        auto to_us = [this](uint64_t cycles) -> double {
            return (static_cast<double>(cycles) / cycles_per_ns_) / 1000.0;
        };

        double avg_cycles = 0;
        for (uint64_t s : sorted) avg_cycles += static_cast<double>(s);
        avg_cycles /= static_cast<double>(n);

        std::cout << std::fixed << std::setprecision(3);
        std::cout << label << " (" << n << " samples):" << std::endl;
        std::cout << "  avg : " << to_us(static_cast<uint64_t>(avg_cycles)) << " us" << std::endl;
        std::cout << "  p50 : " << to_us(sorted[n * 50 / 100])  << " us" << std::endl;
        std::cout << "  p95 : " << to_us(sorted[n * 95 / 100])  << " us" << std::endl;
        std::cout << "  p99 : " << to_us(sorted[n * 99 / 100])  << " us" << std::endl;
        std::cout << "  max : " << to_us(sorted[n - 1])         << " us" << std::endl;
    }

    /** @brief Number of recorded samples. */
    size_t count() const { return samples_.size(); }

private:
    std::vector<uint64_t> samples_;
    double cycles_per_ns_;
};

} // namespace spmc
