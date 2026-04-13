#!/bin/bash

# ==============================================================================
# Script Name: setup_emsdk.sh
# Purpose: Install Emscripten SDK and build antlr4-cppruntime for WASM.
# Output: emsdk/ directory with activated SDK + emsdk/antlr4-wasm/ with headers & lib
# ==============================================================================

set -e

# Define colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMSDK_DIR="$PROJECT_ROOT/emsdk"
ANTLR4_WASM_DIR="$EMSDK_DIR/antlr4-wasm"

# ==============================================================================
# Step 1: Install Emscripten SDK
# ==============================================================================
if [ -f "$EMSDK_DIR/emsdk_env.sh" ]; then
    echo -e "${YELLOW}>>> emsdk already installed, skipping...${NC}"
else
    git config http.postBuffer 524288000
    echo -e "${BLUE}>>> [1/3] Cloning Emscripten SDK...${NC}"
    git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

cd "$EMSDK_DIR"

# ==============================================================================
# Step 2: Install and activate latest version
# ==============================================================================
if [ -f "$EMSDK_DIR/upstream/emscripten/emcc" ]; then
    echo -e "${YELLOW}>>> Emscripten already installed, skipping...${NC}"
else
    echo -e "${BLUE}>>> [2/3] Installing and activating latest Emscripten...${NC}"
    ./emsdk install latest
    ./emsdk activate latest
fi

# Source the environment
source "$EMSDK_DIR/emsdk_env.sh"

# ==============================================================================
# Step 3: Build antlr4-cppruntime for WASM
# ==============================================================================
if [ -f "$ANTLR4_WASM_DIR/lib/libantlr4-runtime.a" ]; then
    echo -e "${YELLOW}>>> antlr4-cppruntime for WASM already built, skipping...${NC}"
else
    echo -e "${BLUE}>>> [3/3] Building antlr4-cppruntime for WASM...${NC}"

    ANTLR4_SRC="$EMSDK_DIR/antlr4-src"

    if [ ! -d "$ANTLR4_SRC" ]; then
        git clone https://github.com/antlr/antlr4.git --branch 4.13.2 --depth 1 "$ANTLR4_SRC"
    fi

    ANTLR4_BUILD="$EMSDK_DIR/antlr4-build"
    mkdir -p "$ANTLR4_BUILD"

    emcmake cmake -S "$ANTLR4_SRC/runtime/Cpp" -B "$ANTLR4_BUILD" \
        -G Ninja \
        -DBUILD_SHARED_LIBS=OFF \
        -DANTLR_BUILD_SHARED=OFF \
        -DANTLR_BUILD_STATIC=ON \
        -DANTLR_BUILD_CPP_TESTS=OFF \
        -DWITH_DEMO=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$ANTLR4_WASM_DIR"

    cmake --build "$ANTLR4_BUILD" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
    cmake --install "$ANTLR4_BUILD"

    echo -e "${GREEN}>>> antlr4-cppruntime installed to $ANTLR4_WASM_DIR${NC}"
fi

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} Emscripten SDK setup complete!                        ${NC}"
echo -e "${GREEN} emsdk:  $EMSDK_DIR                                   ${NC}"
echo -e "${GREEN} antlr4: $ANTLR4_WASM_DIR                             ${NC}"
echo -e "${GREEN}======================================================${NC}"
