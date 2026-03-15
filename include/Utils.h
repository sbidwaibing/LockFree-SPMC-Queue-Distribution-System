#pragma once

/**
 * @file Utils.h
 * @brief Low-level timing utilities for cycle-accurate latency measurement.
 *
 * Provides rdtsc-based timestamping (~10x faster than clock_gettime) and
 * CPU frequency calibration for converting cycle counts to nanoseconds.
 * These are essential for latency profiling in the hot path without
 * introducing system call overhead.
 */

#include <cstdint>
#include <chrono>
#include <thread>
#include <ctime>

// Platform detection for hardware timestamp counter
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define SPMC_ARCH_X86 1
    #include <x86intrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define SPMC_ARCH_ARM64 1
#endif

namespace spmc {

/**
 * @brief Read a high-resolution timestamp with minimal overhead.
 *
 * On x86-64: Uses the RDTSC instruction (~1 cycle, no kernel involvement).
 * On ARM64:  Uses the CNTVCT_EL0 virtual timer counter register.
 * Fallback:  Uses std::chrono (slightly higher overhead due to potential
 *            system call, but portable).
 *
 * @return Current counter value in platform-specific ticks.
 */
inline uint64_t rdtsc() {
#if defined(SPMC_ARCH_X86)
    return __rdtsc();
#elif defined(SPMC_ARCH_ARM64)
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    // Portable fallback: nanoseconds since epoch
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#endif
}

/**
 * @brief Estimate counter frequency in ticks per nanosecond.
 *
 * Calibrates by measuring counter ticks against CLOCK_MONOTONIC over a
 * 500ms window. This conversion factor is needed to translate raw
 * rdtsc() values into human-readable nanoseconds/microseconds.
 *
 * On ARM64, the counter frequency can also be read directly from
 * CNTFRQ_EL0, but calibration works universally.
 *
 * @return Ticks per nanosecond (e.g., ~3.5 on a 3.5 GHz x86 CPU,
 *         ~0.024 on a 24 MHz ARM timer).
 *
 * @note Call once at startup. The 500ms sleep makes this unsuitable
 *       for the hot path.
 */
inline double calibrate_cycles_per_ns() {
    uint64_t t0 = rdtsc();
    timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    uint64_t t1 = rdtsc();

    uint64_t delta_cycles = t1 - t0;
    uint64_t delta_ns = (ts1.tv_sec - ts0.tv_sec) * 1'000'000'000ULL
                      + (ts1.tv_nsec - ts0.tv_nsec);
    return static_cast<double>(delta_cycles) / delta_ns;
}

} // namespace spmc
