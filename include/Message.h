#pragma once

/**
 * @file Message.h
 * @brief Trading message types using a discriminated union.
 *
 * Defines the message format used in the lock-free queue. Uses a tagged
 * union (MessageType enum + anonymous union) instead of inheritance or
 * std::variant for three reasons:
 *   1. No vtable pointer — saves 8 bytes per message, no indirect dispatch
 *   2. Trivially copyable — safe to memcpy across shared memory boundaries
 *   3. Cache-friendly — entire message fits in a single 64-byte cache line
 *
 * Each message is timestamped at construction time using rdtsc() for
 * cycle-accurate end-to-end latency measurement.
 */

#include "Utils.h"
#include <cstdint>

namespace spmc {

/**
 * @brief A new order placed on the book.
 */
struct AddOrder {
    uint64_t order_id;
    double   price;
    uint32_t quantity;
};

/**
 * @brief A matched trade execution.
 */
struct Trade {
    uint64_t trade_id;
    double   price;
    uint32_t quantity;
};

/**
 * @brief Cancellation/removal of an existing order.
 */
struct DeleteOrder {
    uint64_t order_id;
};

/**
 * @brief Discriminator for the Message union.
 *
 * Stored as uint8_t to minimize padding. Used in switch-case dispatch
 * which the compiler can optimize into a jump table — faster than
 * virtual dispatch or std::visit.
 */
enum class MessageType : uint8_t {
    Add,
    Trade,
    Delete
};

/**
 * @brief A single message in the queue.
 *
 * Layout:
 *   - type         (1 byte)  — which union member is active
 *   - [padding]    (7 bytes) — alignment for timestamp_ns
 *   - timestamp_ns (8 bytes) — rdtsc value at publish time
 *   - union        (24 bytes max, AddOrder is largest)
 *
 * Total: ~40 bytes, fits within a 64-byte cache line alongside the
 * Slot's atomic sequence number.
 *
 * Trivially copyable: safe for shared memory, no heap pointers,
 * no virtual methods.
 */
struct Message {
    MessageType type;
    uint64_t    timestamp_ns;

    union {
        AddOrder    add;
        Trade       trade;
        DeleteOrder del;
    };

    Message() = default;
    ~Message() = default;

    /** @brief Construct an AddOrder message, timestamped now. */
    Message(const AddOrder& a)
        : type(MessageType::Add), timestamp_ns(rdtsc()), add(a) {}

    /** @brief Construct a Trade message, timestamped now. */
    Message(const Trade& t)
        : type(MessageType::Trade), timestamp_ns(rdtsc()), trade(t) {}

    /** @brief Construct a DeleteOrder message, timestamped now. */
    Message(const DeleteOrder& d)
        : type(MessageType::Delete), timestamp_ns(rdtsc()), del(d) {}
};

} // namespace spmc
