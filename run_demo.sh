#!/bin/bash
# run_demo.sh — Run the full demo inside Docker without multiple terminals
#
# Usage:
#   chmod +x run_demo.sh
#   ./run_demo.sh          # Run tests + full demo
#   ./run_demo.sh test     # Run only tests
#   ./run_demo.sh demo     # Run only the producer/consumer demo

set -e

IMAGE_NAME="lockfree-spmc-queue"
CONTAINER_NAME="lockfree-demo"

# ── Build ─────────────────────────────────────────────
echo "========================================"
echo " Building Docker image..."
echo "========================================"
docker build -t $IMAGE_NAME .

# ── Tests ─────────────────────────────────────────────
if [ "$1" != "demo" ]; then
    echo ""
    echo "========================================"
    echo " Running unit tests..."
    echo "========================================"
    docker run --rm $IMAGE_NAME ./build/test_queue
    echo ""
fi

if [ "$1" == "test" ]; then
    exit 0
fi

# ── Demo ──────────────────────────────────────────────
echo "========================================"
echo " Running producer + 3 consumers demo"
echo "========================================"
echo ""

# Remove stale container from any previous failed run
docker rm -f $CONTAINER_NAME 2>/dev/null || true

docker run --rm --name $CONTAINER_NAME \
    --ipc=host \
    -v /dev/shm:/dev/shm \
    $IMAGE_NAME \
    bash -c '
        # Step 1: Start producer with --auto flag.
        # It creates shared memory, waits 4 seconds for consumers, then publishes.
        ./build/lockfree_queue p --auto 4 &
        PID_PRODUCER=$!

        # Step 2: Wait for shared memory to be created (~instant).
        sleep 0.5

        # Step 3: Start consumers. They calibrate (~500ms) then register.
        echo "[Demo] Starting 3 consumers..."
        ./build/lockfree_queue c spin  &
        ./build/lockfree_queue c yield &
        ./build/lockfree_queue c block &

        # Step 4: Wait for everything to finish.
        # Producer auto-starts after 4s, consumers process 1M messages each.
        wait

        echo ""
        echo "[Demo] Cleaning up shared memory..."
        ./build/lockfree_queue x

        echo ""
        echo "========================================"
        echo " Demo complete!"
        echo "========================================"
    '
