#!/bin/bash
# Build Fizzik DSP for Move (ARM64) via Docker.
# Uses the docker create + docker cp pattern (robust on Windows/MSYS) with an
# explicit exit-code check (set -e does NOT propagate docker start failures on Git Bash).
set -e

MODULE_ID="fizzik"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WIN_ROOT="$(cd "$ROOT" && pwd -W 2>/dev/null || echo "$ROOT")"

echo "Building $MODULE_ID (ARM64)..."
docker build -t fizzik-builder "$SCRIPT_DIR"

mkdir -p "$ROOT/dist/$MODULE_ID"
cp "$ROOT/src/module.json" "$ROOT/dist/$MODULE_ID/"
cp "$ROOT/src/help.json" "$ROOT/dist/$MODULE_ID/"

COMPILE="aarch64-linux-gnu-gcc -O2 -ffast-math -shared -fPIC \
  -o /build/dist/$MODULE_ID/dsp.so /build/src/dsp/$MODULE_ID.c -lm && \
  tar -czf /build/dist/$MODULE_ID-module.tar.gz -C /build/dist $MODULE_ID"

CONTAINER_ID=$(MSYS_NO_PATHCONV=1 docker create -w /build fizzik-builder bash -c "$COMPILE")
docker cp "$WIN_ROOT/src" "$CONTAINER_ID:/build/src"
docker cp "$WIN_ROOT/dist" "$CONTAINER_ID:/build/dist"
docker start -a "$CONTAINER_ID"

EXIT_CODE=$(docker inspect "$CONTAINER_ID" --format='{{.State.ExitCode}}')
if [ "$EXIT_CODE" != "0" ]; then
    echo "ERROR: Compile failed (exit $EXIT_CODE)."
    docker rm "$CONTAINER_ID" > /dev/null
    exit 1
fi

docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID/dsp.so" "$WIN_ROOT/dist/$MODULE_ID/"
docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID-module.tar.gz" "$WIN_ROOT/dist/"
docker rm "$CONTAINER_ID" > /dev/null

echo "Built: dist/$MODULE_ID-module.tar.gz"
echo "Verify: strings dist/$MODULE_ID/dsp.so | grep move_plugin_init_v2"
