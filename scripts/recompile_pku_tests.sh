#!/usr/bin/env bash

set -euo pipefail

TESTSUITS_DIR="${1:-$HOME/OS/testsuits-for-oskernel}"
TESTING_DIR="$TESTSUITS_DIR/riscv-syscalls-testing"
BUILD_DIR="$TESTING_DIR/user/build"

if [ ! -d "$TESTSUITS_DIR" ]; then
  echo "testsuits directory not found: $TESTSUITS_DIR" >&2
  exit 1
fi

if [ ! -d "$TESTING_DIR/user" ]; then
  echo "missing directory: $TESTING_DIR/user" >&2
  exit 1
fi

rm -rf "$BUILD_DIR" "$TESTING_DIR/user/riscv64"

docker run -ti --rm \
  -v "$TESTING_DIR:/testing" \
  -w /testing/user \
  --privileged=true \
  docker.educg.net/cg/os-contest:2024p6 \
  /bin/bash -c "sh build-oscomp.sh"

rm -rf riscv64
cp -r "$BUILD_DIR/riscv64" ./riscv64

echo "updated riscv64 tests in $(pwd)/riscv64"
