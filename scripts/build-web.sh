#!/usr/bin/env bash
# Build the browser version (WebAssembly + WebGL 2) with Emscripten.
# Needs the emsdk: https://emscripten.org/docs/getting_started/downloads.html
#   source /path/to/emsdk/emsdk_env.sh
# Produces build-web/alone.html (+ .js, .wasm, .data). Serve the folder over
# HTTP, e.g.  scripts/build-web.sh --serve  and open http://localhost:8080/alone.html
set -euo pipefail
cd "$(dirname "$0")/.."
command -v emcmake >/dev/null || { echo "emcmake not found: source emsdk_env.sh first"; exit 1; }
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web -j"$(nproc)"
if [[ "${1:-}" == "--serve" ]]; then
    cd build-web && python3 -m http.server 8080
fi
