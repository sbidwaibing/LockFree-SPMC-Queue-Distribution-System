cmake_minimum_required(VERSION 3.10)
project(lockfree_spmc_queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization flags for release builds
# -march=native is omitted for Docker cross-arch compatibility
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Include directory
include_directories(${CMAKE_SOURCE_DIR}/include)

# Main executable
add_executable(lockfree_queue
    src/main.cpp
)
target_link_libraries(lockfree_queue PRIVATE pthread rt)

# Test executable
add_executable(test_queue
    tests/test_queue.cpp
)
target_link_libraries(test_queue PRIVATE pthread rt)

# CTest integration
enable_testing()
add_test(NAME unit_tests COMMAND test_queue)
