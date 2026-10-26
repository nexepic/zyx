#!/bin/bash

# ==============================================================================
# Script Name: build_wasm.sh
# Purpose: Build ZYX as a WebAssembly module for browser-based Cypher playground.
# Output: build_wasm/zyx.js + zyx-filesystem.mjs + zyx.wasm
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
INPUTXX_LIB="$BUILD_DIR/libzyx_inputxx.a"

if [ ! -f "$ZYX_CORE_LIB" ]; then
    echo -e "${RED}Error: $ZYX_CORE_LIB not found${NC}"
    exit 1
fi
if [ ! -f "$INPUTXX_LIB" ]; then
    echo -e "${RED}Error: $INPUTXX_LIB not found${NC}"
    exit 1
fi

link_wasm_module() {
em++ \
    -o "$BUILD_DIR/zyx.js" \
    -Wl,--whole-archive "$ZYX_CORE_LIB" "$INPUTXX_LIB" -Wl,--no-whole-archive \
    -L"$ANTLR4_WASM_DIR/lib" -lantlr4-runtime \
    -sEXPORTED_FUNCTIONS='[
        "_zyx_driver_abi_version_major",
        "_zyx_driver_abi_version_minor",
        "_zyx_driver_abi_version_patch",
        "_zyx_driver_runtime_version",
        "_zyx_driver_error_code",
        "_zyx_driver_error_message",
        "_zyx_driver_error_free",
        "_zyx_driver_db_open",
        "_zyx_driver_db_open_if_exists",
        "_zyx_driver_db_close",
        "_zyx_driver_txn_begin_read_only",
        "_zyx_driver_txn_execute",
        "_zyx_driver_txn_rollback",
        "_zyx_driver_txn_close",
        "_zyx_driver_result_free",
        "_zyx_driver_result_next",
        "_zyx_driver_result_column_count",
        "_zyx_driver_result_column_name",
        "_zyx_driver_result_value_type",
        "_zyx_driver_result_get_int64",
        "_zyx_driver_result_get_double",
        "_zyx_driver_result_get_bool",
        "_zyx_driver_result_get_string",
        "_zyx_driver_result_get_list_count",
        "_zyx_driver_result_get_list_value_type",
        "_zyx_driver_result_get_list_int64",
        "_zyx_driver_result_get_list_double",
        "_zyx_driver_result_get_list_bool",
        "_zyx_driver_result_get_list_string",
        "_zyx_driver_result_get_value",
        "_zyx_driver_value_ref_type",
        "_zyx_driver_value_ref_get_int64",
        "_zyx_driver_value_ref_get_double",
        "_zyx_driver_value_ref_get_bool",
        "_zyx_driver_value_ref_get_string",
        "_zyx_driver_value_ref_list_count",
        "_zyx_driver_value_ref_list_get",
        "_zyx_driver_value_ref_list_get_int64",
        "_zyx_driver_value_ref_list_get_double",
        "_zyx_driver_value_ref_list_get_bool",
        "_zyx_driver_value_ref_list_get_string",
        "_zyx_driver_value_ref_map_count",
        "_zyx_driver_value_ref_map_key",
        "_zyx_driver_value_ref_map_get",
        "_zyx_driver_value_ref_map_get_int64",
        "_zyx_driver_value_ref_map_get_double",
        "_zyx_driver_value_ref_map_get_bool",
        "_zyx_driver_value_ref_map_get_string",
        "_zyx_driver_value_ref_get_node_id",
        "_zyx_driver_value_ref_get_node_label_count",
        "_zyx_driver_value_ref_get_node_label",
        "_zyx_driver_value_ref_get_edge_id",
        "_zyx_driver_value_ref_get_edge_source_id",
        "_zyx_driver_value_ref_get_edge_target_id",
        "_zyx_driver_value_ref_get_edge_type",
        "_zyx_driver_value_ref_get_entity_properties_json",
        "_zyx_driver_result_get_node_id",
        "_zyx_driver_result_get_node_label_count",
        "_zyx_driver_result_get_node_label",
        "_zyx_driver_result_get_edge_id",
        "_zyx_driver_result_get_edge_source_id",
        "_zyx_driver_result_get_edge_target_id",
        "_zyx_driver_result_get_edge_type",
        "_zyx_driver_result_get_entity_properties_json",
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
    -O1 --no-binaryen-passes \
    -fwasm-exceptions
}

if ! link_wasm_module; then
    if [ "$(uname -s)" != "Darwin" ]; then
        exit 1
    fi

    # Some macOS hosts kill Emscripten's llvm-objcopy during custom-section
    # cleanup. Retry only there with wasm-opt, keeping the compiler/linker intact.
    FALLBACK_LLVM_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/zyx-wasm-llvm.XXXXXX")"
    cleanup_fallback_tools() {
        rm -rf "$FALLBACK_LLVM_ROOT"
    }
    trap cleanup_fallback_tools EXIT

    for tool in clang clang++ llvm-ar llvm-ranlib llvm-nm llvm-ld wasm-ld; do
        ln -s "$EMSDK_DIR/upstream/bin/$tool" "$FALLBACK_LLVM_ROOT/$tool"
    done

    cat > "$FALLBACK_LLVM_ROOT/llvm-objcopy" <<'EOF'
#!/bin/sh
set -eu

input_path=''
output_path=''
for argument in "$@"; do
    case "$argument" in
        --remove-section=*) ;;
        *)
            if [ -z "$input_path" ]; then
                input_path="$argument"
            else
                output_path="$argument"
            fi
            ;;
    esac
done

if [ -z "$input_path" ] || [ -z "$output_path" ]; then
    echo "unsupported llvm-objcopy arguments" >&2
    exit 2
fi

temporary_path="${output_path}.wasm-opt-tmp"
"$ZYX_WASM_OPT" "$input_path" -O0 --strip-debug -o "$temporary_path"
mv "$temporary_path" "$output_path"
EOF
    chmod 755 "$FALLBACK_LLVM_ROOT/llvm-objcopy"
    export ZYX_WASM_OPT="$EMSDK_DIR/upstream/bin/wasm-opt"
    echo -e "${YELLOW}>>> Retrying WASM link with macOS objcopy compatibility fallback...${NC}"
    EM_LLVM_ROOT="$FALLBACK_LLVM_ROOT" link_wasm_module
fi

# Keep the generated filesystem and bootstrap in separate bounded modules.
python3 "$PROJECT_ROOT/scripts/split_wasm_runtime.py" "$BUILD_DIR/zyx.js"

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} WASM Build Successful!                                ${NC}"
echo -e "${GREEN} Output: $BUILD_DIR/zyx.js                             ${NC}"
echo -e "${GREEN}         $BUILD_DIR/zyx.wasm                           ${NC}"
echo -e "${GREEN}======================================================${NC}"

ls -lh "$BUILD_DIR/zyx.js" "$BUILD_DIR/zyx-filesystem.mjs" "$BUILD_DIR/zyx.wasm"
