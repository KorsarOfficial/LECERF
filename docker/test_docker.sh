#!/usr/bin/env bash
# Local verification: build, size-check, run, assert pytest pass.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG="lecerf:dev"
MAX_BYTES=$((50 * 1024 * 1024))

echo "=== Step 1: build image ==="
docker build -t "$IMG" -f docker/Dockerfile .

echo "=== Step 2: size gate ==="
size=$(docker image inspect "$IMG" --format='{{.Size}}')
echo "image size: $size bytes"
if [ "$size" -gt "$MAX_BYTES" ]; then
    echo "FAIL: image exceeds 50 MB (got $size bytes)"
    exit 1
fi
echo "PASS: under 50 MB"

echo "=== Step 3: --version ==="
out=$(docker run --rm "$IMG" --version)
echo "$out"
echo "$out" | grep -q "lecerf 0.1.0" || { echo "FAIL version output"; exit 1; }

echo "=== Step 4: --help ==="
docker run --rm "$IMG" --help | grep -q "lecerf-runner" || \
    { echo "FAIL help output"; exit 1; }

echo "=== Step 5: pytest run ==="
MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$(pwd)/firmware":/fw:ro \
    -v "$(pwd)/python/tests":/tests \
    "$IMG" /fw /tests

echo "=== ALL DOCKER GATES PASSED ==="
