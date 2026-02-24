#!/bin/bash
# HaloSound - Build Script
#
# Usage:
#   ./build.sh wasm     - Build WASM DSP module (requires Emscripten)
#   ./build.sh server   - Install server dependencies
#   ./build.sh app      - Package webOS app (requires webOS SDK)
#   ./build.sh all      - Build everything
#   ./build.sh clean    - Clean build artifacts

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

build_wasm() {
    echo "=== Building WASM DSP Module ==="
    cd "$SCRIPT_DIR/dsp"

    if ! command -v emcc &> /dev/null; then
        echo "Error: Emscripten (emcc) not found."
        echo "Install from: https://emscripten.org/docs/getting_started/downloads.html"
        echo "Then run: source emsdk_env.sh"
        exit 1
    fi

    mkdir -p build
    emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release
    emmake cmake --build build --config Release

    # Copy WASM artifacts to app
    cp build/halosound_dsp.wasm "$SCRIPT_DIR/app/wasm/"
    cp build/halosound_dsp.js "$SCRIPT_DIR/app/wasm/"

    echo "WASM build complete. Artifacts in dsp/build/ and app/wasm/"
}

build_server() {
    echo "=== Installing Server Dependencies ==="
    cd "$SCRIPT_DIR/server"
    npm install
    echo "Server dependencies installed."
}

build_app() {
    echo "=== Packaging webOS App ==="
    cd "$SCRIPT_DIR/app"

    if ! command -v ares-package &> /dev/null; then
        echo "Error: webOS SDK (ares-package) not found."
        echo "Install from: https://www.webosose.org/docs/tools/sdk/cli/cli-user-guide/"
        exit 1
    fi

    # Ensure WASM files exist
    if [ ! -f "wasm/halosound_dsp.wasm" ]; then
        echo "Warning: WASM not built yet. Run './build.sh wasm' first."
    fi

    ares-package .
    echo "App packaged. IPK file created in app/"
}

clean() {
    echo "=== Cleaning ==="
    rm -rf "$SCRIPT_DIR/dsp/build"
    rm -f "$SCRIPT_DIR/app/wasm/halosound_dsp.wasm"
    rm -f "$SCRIPT_DIR/app/wasm/halosound_dsp.js"
    rm -rf "$SCRIPT_DIR/server/node_modules"
    rm -f "$SCRIPT_DIR/app/"*.ipk
    echo "Clean complete."
}

case "${1:-all}" in
    wasm)   build_wasm ;;
    server) build_server ;;
    app)    build_app ;;
    all)
        build_wasm
        build_server
        build_app
        ;;
    clean)  clean ;;
    *)
        echo "Usage: $0 {wasm|server|app|all|clean}"
        exit 1
        ;;
esac
