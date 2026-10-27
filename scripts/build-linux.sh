#!/usr/bin/env bash
# Build Alone on Linux. Needs cmake, a C++17 compiler and the X11/OpenGL
# development headers (see README). raylib is downloaded automatically.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
echo
echo "Done. Run the game with: ./build/alone"
