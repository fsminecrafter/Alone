#!/usr/bin/env bash
# Cross-compile a Windows build of Alone from Linux with MinGW-w64
# (sudo apt install mingw-w64). Produces build-windows/alone.exe + data/.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows -j"$(nproc)"
echo
echo "Done. Copy build-windows/alone.exe and build-windows/data/ to a Windows PC."
