#!/usr/bin/env bash
# Compila Lux con el cmake del sistema. El binario queda en build/lux.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$HERE/build"

mkdir -p "$BUILD_DIR"
cmake -S "$HERE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "listo: $BUILD_DIR/lux"
