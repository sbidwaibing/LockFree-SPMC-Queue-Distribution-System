FROM ubuntu:24.04

# Install build essentials
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Copy project into container
WORKDIR /app
COPY include/ include/
COPY src/ src/
COPY tests/ tests/
COPY CMakeLists.txt .

# Build everything
RUN mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release .. \
    && make -j$(nproc)

# Default: run tests
CMD ["./build/test_queue"]
