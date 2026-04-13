#!/bin/bash

# ==============================================================================
# Script Name: build_wasm.sh
# Purpose: Build ZYX as a WebAssembly module for browser-based Cypher playground.
# Output: build_wasm/zyx.js + build_wasm/zyx.wasm
# ==============================================================================

set -e

# Define colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build_wasm"
EMSDK_DIR="$PROJECT_ROOT/emsdk"
ANTLR4_WASM_DIR="$EMSDK_DIR/antlr4-wasm"

# ==============================================================================
# Step 1: Check prerequisites
# ==============================================================================
echo -e "${BLUE}>>> [1/5] Checking prerequisites...${NC}"

if [ ! -f "$EMSDK_DIR/emsdk_env.sh" ]; then
    echo -e "${RED}Error: emsdk not found. Run ./scripts/setup_emsdk.sh first.${NC}"
    exit 1
fi

if [ ! -f "$ANTLR4_WASM_DIR/lib/libantlr4-runtime.a" ]; then
    echo -e "${RED}Error: antlr4-cppruntime for WASM not found. Run ./scripts/setup_emsdk.sh first.${NC}"
    exit 1
fi

source "$EMSDK_DIR/emsdk_env.sh"

# ==============================================================================
# Step 2: Clean build directory
# ==============================================================================
echo -e "${BLUE}>>> [2/5] Preparing build directory...${NC}"

if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

# ==============================================================================
# Step 3: Generate pkg-config files
# ==============================================================================
echo -e "${BLUE}>>> [3/5] Generating pkg-config files...${NC}"

PC_DIR="$BUILD_DIR/pkgconfig"
mkdir -p "$PC_DIR"

# antlr4-cppruntime pkg-config
cat > "$PC_DIR/antlr4-cppruntime.pc" << EOF
prefix=$ANTLR4_WASM_DIR
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: antlr4-cppruntime
Description: ANTLR4 C++ Runtime (WASM build)
Version: 4.13.2
Libs: -L\${libdir} -lantlr4-runtime
Cflags: -I\${includedir} -I\${includedir}/antlr4-runtime
EOF

# zlib pkg-config (provided by Emscripten's -sUSE_ZLIB=1)
cat > "$PC_DIR/zlib.pc" << EOF
Name: zlib
Description: zlib (Emscripten built-in)
Version: 1.2.13
Libs: -sUSE_ZLIB=1
Cflags: -sUSE_ZLIB=1
EOF

# CLI11 stub (headers only, not needed for WASM library build but satisfies dependency)
cat > "$PC_DIR/CLI11.pc" << EOF
Name: CLI11
Description: CLI11 stub for WASM build
Version: 2.4.1
Cflags:
EOF

# ==============================================================================
# Step 4: Configure with Meson
# ==============================================================================
echo -e "${BLUE}>>> [4/5] Configuring Meson build...${NC}"

meson setup "$BUILD_DIR" \
    --cross-file "$PROJECT_ROOT/compiler_options_wasm.ini" \
    -Dbuildtype=release \
    -Ddefault_library=static \
    -Dtests=disabled \
    --pkg-config-path="$PC_DIR"

# ==============================================================================
# Step 5: Compile
# ==============================================================================
echo -e "${BLUE}>>> [5/5] Compiling...${NC}"

meson compile -C "$BUILD_DIR"

# ==============================================================================
# Step 6: Link final WASM output
# ==============================================================================
echo -e "${BLUE}>>> Linking WASM module...${NC}"

# Find the static library
ZYX_CORE_LIB="$BUILD_DIR/src/libzyx_core.a"
CYPHER_LIB="$BUILD_DIR/src/query/parser/cypher/libcypher_parser.a"

if [ ! -f "$ZYX_CORE_LIB" ]; then
    echo -e "${RED}Error: $ZYX_CORE_LIB not found${NC}"
    exit 1
fi

em++ \
    -o "$BUILD_DIR/zyx.js" \
    "$ZYX_CORE_LIB" \
    "$CYPHER_LIB" \
    -L"$ANTLR4_WASM_DIR/lib" -lantlr4-runtime \
    -sEXPORTED_FUNCTIONS='[
        "_zyx_open",
        "_zyx_open_if_exists",
        "_zyx_close",
        "_zyx_get_last_error",
        "_zyx_execute",
        "_zyx_execute_params",
        "_zyx_result_close",
        "_zyx_begin_transaction",
        "_zyx_begin_read_only_transaction",
        "_zyx_txn_is_read_only",
        "_zyx_txn_execute",
        "_zyx_txn_execute_params",
        "_zyx_txn_commit",
        "_zyx_txn_rollback",
        "_zyx_txn_close",
        "_zyx_params_create",
        "_zyx_params_set_int",
        "_zyx_params_set_double",
        "_zyx_params_set_string",
        "_zyx_params_set_bool",
        "_zyx_params_set_null",
        "_zyx_params_close",
        "_zyx_create_node",
        "_zyx_create_node_ret_id",
        "_zyx_create_edge_by_id",
        "_zyx_result_next",
        "_zyx_result_column_count",
        "_zyx_result_column_name",
        "_zyx_result_get_duration",
        "_zyx_result_get_type",
        "_zyx_result_get_int",
        "_zyx_result_get_double",
        "_zyx_result_get_bool",
        "_zyx_result_get_string",
        "_zyx_result_get_node",
        "_zyx_result_get_edge",
        "_zyx_result_get_props_json",
        "_zyx_result_list_size",
        "_zyx_result_list_get_type",
        "_zyx_result_list_get_int",
        "_zyx_result_list_get_double",
        "_zyx_result_list_get_bool",
        "_zyx_result_list_get_string",
        "_zyx_result_get_map_json",
        "_zyx_result_is_success",
        "_zyx_result_get_error",
        "_malloc",
        "_free"
    ]' \
    -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS","HEAPU8","HEAP32","UTF8ToString","getValue"]' \
    -sALLOW_MEMORY_GROWTH=1 \
    -sNO_EXIT_RUNTIME=1 \
    -sUSE_ZLIB=1 \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=createZyxModule \
    -sERROR_ON_UNDEFINED_SYMBOLS=0 \
    -O2 --no-binaryen-passes

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} WASM Build Successful!                                ${NC}"
echo -e "${GREEN} Output: $BUILD_DIR/zyx.js                             ${NC}"
echo -e "${GREEN}         $BUILD_DIR/zyx.wasm                           ${NC}"
echo -e "${GREEN}======================================================${NC}"

ls -lh "$BUILD_DIR/zyx.js" "$BUILD_DIR/zyx.wasm"
