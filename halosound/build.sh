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
    echo "=== Building WASM DSP Module (with libmysofa) ==="

    if ! command -v emcc &> /dev/null; then
        echo "Error: Emscripten (emcc) not found."
        echo "Install from: https://emscripten.org/docs/getting_started/downloads.html"
        echo "Then run: source emsdk_env.sh"
        exit 1
    fi

    # libmysofa must be statically linked so real SOFA HRTF filtering happens
    # on the client (otherwise sofa_loader falls back to a dirac-ITD stub that
    # ignores the profile entirely).  build-wasm.sh compiles it from source.
    MYSOFA_ROOT="${MYSOFA_ROOT:-$HOME/libmysofa}"
    if [ ! -d "$MYSOFA_ROOT/src/hrtf" ]; then
        echo "Cloning libmysofa into $MYSOFA_ROOT ..."
        git clone --depth 1 https://github.com/hoene/libmysofa.git "$MYSOFA_ROOT"
    fi

    bash "$SCRIPT_DIR/build-wasm.sh" "$MYSOFA_ROOT"
    echo "WASM build complete (SIMD + scalar) in app/wasm/"
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
