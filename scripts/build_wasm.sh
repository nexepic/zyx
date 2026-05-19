#!/bin/bash

# ==============================================================================
# Script Name: build_wasm.sh
# Purpose: Build ZYX as a WebAssembly module for browser-based Cypher playground.
# Output: build_wasm/zyx.js + build_wasm/zyx.wasm
# ==============================================================================

set -e

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build_wasm"
EMSDK_DIR="$PROJECT_ROOT/emsdk"
ANTLR4_WASM_DIR="$EMSDK_DIR/antlr4-wasm"

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

echo -e "${BLUE}>>> [2/5] Preparing build directory...${NC}"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

export ZYX_ANTLR4_WASM_DIR="$ANTLR4_WASM_DIR"

echo -e "${BLUE}>>> [3/5] Configuring CMake build...${NC}"
emcmake cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DZYX_WASM=ON \
    -DZYX_BUILD_TESTS=OFF \
    -DZYX_BUILD_APPS=OFF \
    -DZYX_BUILD_PYTHON=OFF \
    -DBUILD_SHARED_LIBS=OFF

echo -e "${BLUE}>>> [4/5] Compiling static libraries...${NC}"
cmake --build "$BUILD_DIR" --target zyx_core

echo -e "${BLUE}>>> [5/5] Linking WASM module...${NC}"

ZYX_CORE_LIB="$BUILD_DIR/libzyx_core.a"
CYPHER_LIB="$BUILD_DIR/libzyx_cypher_parser.a"
INPUTXX_LIB="$BUILD_DIR/libzyx_inputxx.a"

if [ ! -f "$ZYX_CORE_LIB" ]; then
    echo -e "${RED}Error: $ZYX_CORE_LIB not found${NC}"
    exit 1
fi
if [ ! -f "$CYPHER_LIB" ]; then
    echo -e "${RED}Error: $CYPHER_LIB not found${NC}"
    exit 1
fi
if [ ! -f "$INPUTXX_LIB" ]; then
    echo -e "${RED}Error: $INPUTXX_LIB not found${NC}"
    exit 1
fi

em++ \
    -o "$BUILD_DIR/zyx.js" \
    -Wl,--whole-archive "$ZYX_CORE_LIB" "$CYPHER_LIB" "$INPUTXX_LIB" -Wl,--no-whole-archive \
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
        "_zyx_params_set_list",
        "_zyx_params_set_map",
        "_zyx_list_create",
        "_zyx_list_push_int",
        "_zyx_list_push_double",
        "_zyx_list_push_string",
        "_zyx_list_push_bool",
        "_zyx_list_push_null",
        "_zyx_list_push_list",
        "_zyx_list_close",
        "_zyx_map_create",
        "_zyx_map_set_int",
        "_zyx_map_set_double",
        "_zyx_map_set_string",
        "_zyx_map_set_bool",
        "_zyx_map_set_null",
        "_zyx_map_set_list",
        "_zyx_map_set_map",
        "_zyx_map_close",
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
    -O2 --no-binaryen-passes \
    -fwasm-exceptions

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} WASM Build Successful!                                ${NC}"
echo -e "${GREEN} Output: $BUILD_DIR/zyx.js                             ${NC}"
echo -e "${GREEN}         $BUILD_DIR/zyx.wasm                           ${NC}"
echo -e "${GREEN}======================================================${NC}"

ls -lh "$BUILD_DIR/zyx.js" "$BUILD_DIR/zyx.wasm"
