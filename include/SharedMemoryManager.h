#pragma once

/**
 * @file SharedMemoryManager.h
 * @brief RAII manager for POSIX shared memory and eventfd lifecycle.
 *
 * Wraps shm_open, ftruncate, mmap, munmap, and shm_unlink into a class
 * that guarantees cleanup on destruction. Solves several problems in the
 * original implementation:
 *   - File descriptors were never closed after mmap
 *   - No munmap on error paths or at shutdown
 *   - Consumer processes used reinterpret_cast on mmap'd memory without
 *     placement new (undefined behavior)
 *   - No centralized ownership of the shared memory lifecycle
 *
 * Also manages an eventfd for the BlockingWaitStrategy, since the fd
 * needs to be shared between the producer and all blocking consumers.
 *
 * Usage:
 *   // Producer (creates the shared memory):
 *   SharedMemoryManager<Message> mgr("/my_queue", true);
 *   auto* queue = mgr.get_queue();
 *
 *   // Consumer (attaches to existing shared memory):
 *   SharedMemoryManager<Message> mgr("/my_queue", false);
 *   auto* queue = mgr.get_queue();
 *
 *   // Both: destructor handles cleanup automatically.
 */

#include "Queue.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace spmc {

/**
 * @brief Manages the shared memory segment containing the queue.
 *
 * @tparam T Message type used in the queue.
 */
template <typename T>
class SharedMemoryManager {
public:
    /**
     * @brief Open or create a shared memory segment containing the queue.
     *
     * @param name    POSIX shared memory name (must start with '/').
     * @param create  If true, creates and initializes the segment.
     *                If false, attaches to an existing segment.
     *
     * @throws std::runtime_error on any system call failure.
     */
    SharedMemoryManager(const std::string& name, bool create)
        : name_(name), is_owner_(create) {

        // Step 1: Open or create the shared memory object
        int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
        shm_fd_ = shm_open(name_.c_str(), flags, 0666);
        if (shm_fd_ == -1) {
            throw std::runtime_error(
                "shm_open failed: " + std::string(std::strerror(errno)));
        }

        // Step 2: Set the size (only when creating)
        size_ = sizeof(LockFreePubSubQueue<T>);
        if (create) {
            if (ftruncate(shm_fd_, static_cast<off_t>(size_)) != 0) {
                close(shm_fd_);
                shm_unlink(name_.c_str());
                throw std::runtime_error(
                    "ftruncate failed: " + std::string(std::strerror(errno)));
            }
        }

        // Step 3: Memory-map the shared memory into our address space
        mapped_ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                           MAP_SHARED, shm_fd_, 0);
        if (mapped_ptr_ == MAP_FAILED) {
            close(shm_fd_);
            if (create) shm_unlink(name_.c_str());
            throw std::runtime_error(
                "mmap failed: " + std::string(std::strerror(errno)));
        }

        // Step 4: Close the fd — mmap holds its own reference.
        // The mapping persists even after close().
        close(shm_fd_);
        shm_fd_ = -1;

        // Step 5: Construct the queue in shared memory (producer only).
        // Consumers reinterpret the already-constructed memory.
        if (create) {
            queue_ = new (mapped_ptr_) LockFreePubSubQueue<T>();
        } else {
            queue_ = reinterpret_cast<LockFreePubSubQueue<T>*>(mapped_ptr_);
        }

        // Step 6: Create eventfd for BlockingWaitStrategy.
        // EFD_NONBLOCK: reads return EAGAIN instead of blocking if counter is 0
        // EFD_SEMAPHORE: each read() decrements by 1 (not drain-to-zero)
        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
        if (event_fd_ == -1) {
            munmap(mapped_ptr_, size_);
            if (create) shm_unlink(name_.c_str());
            throw std::runtime_error(
                "eventfd failed: " + std::string(std::strerror(errno)));
        }
    }

    // Non-copyable (owns system resources)
    SharedMemoryManager(const SharedMemoryManager&) = delete;
    SharedMemoryManager& operator=(const SharedMemoryManager&) = delete;

    // Movable
    SharedMemoryManager(SharedMemoryManager&& other) noexcept
        : name_(std::move(other.name_))
        , is_owner_(other.is_owner_)
        , shm_fd_(other.shm_fd_)
        , mapped_ptr_(other.mapped_ptr_)
        , size_(other.size_)
        , queue_(other.queue_)
        , event_fd_(other.event_fd_) {
        other.mapped_ptr_ = nullptr;
        other.queue_ = nullptr;
        other.event_fd_ = -1;
        other.shm_fd_ = -1;
    }

    /**
     * @brief Destructor — unmap memory, close eventfd, unlink if owner.
     *
     * The owner (producer) unlinks the shared memory segment, removing
     * it from /dev/shm. Consumers only unmap their local mapping.
     */
    ~SharedMemoryManager() {
        if (event_fd_ != -1) {
            close(event_fd_);
        }
        if (mapped_ptr_ && mapped_ptr_ != MAP_FAILED) {
            munmap(mapped_ptr_, size_);
        }
        if (is_owner_) {
            shm_unlink(name_.c_str());
        }
    }

    /** @brief Get the queue constructed in shared memory. */
    LockFreePubSubQueue<T>* get_queue() { return queue_; }

    /** @brief Get the eventfd for BlockingWaitStrategy. */
    int get_event_fd() const { return event_fd_; }

    /**
     * @brief Unlink the shared memory segment manually.
     *
     * Useful for explicit cleanup (the 'x' command in main).
     * After unlinking, existing mappings remain valid but no new
     * processes can attach.
     */
    static void unlink(const std::string& name) {
        shm_unlink(name.c_str());
    }

private:
    std::string             name_;
    bool                    is_owner_;
    int                     shm_fd_    = -1;
    void*                   mapped_ptr_ = nullptr;
    size_t                  size_       = 0;
    LockFreePubSubQueue<T>* queue_     = nullptr;
    int                     event_fd_  = -1;
};

} // namespace spmc
