#!/bin/bash

# ==============================================================================
# Script Name: build_release.sh
# Purpose: Build optimized shared libraries and generate the SDK artifact.
# Output: build_release/dist and build_release/zyx-sdk-<ver>-<os>-<arch>.tar.gz
# ==============================================================================

set -e

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

BUILD_DIR="build_release"
DIST_DIR="$BUILD_DIR/dist"
ARTIFACT_NAME="zyx-sdk"
VERSION="$(python3 scripts/read_project_version.py)"

if [[ "$OSTYPE" == "darwin"* ]]; then
    OS_TAG="macos"
elif [[ "$OSTYPE" == "msys"* || "$OSTYPE" == "win32"* ]]; then
    OS_TAG="windows"
else
    OS_TAG="linux"
fi

ARCH_TAG=$(uname -m)
FULL_PACKAGE_NAME="${ARTIFACT_NAME}-${VERSION}-${OS_TAG}-${ARCH_TAG}"

echo -e "${BLUE}>>> [1/5] Cleaning release environment...${NC}"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo -e "${BLUE}>>> [2/5] Installing dependencies (Release)...${NC}"
conan install . \
    --output-folder="$BUILD_DIR" \
    --build=missing \
    --build="antlr4-cppruntime/*" \
    -s build_type=Release \
    -s compiler.cppstd=20 \
    -o '*:shared=False' \
    -o '*:fPIC=True' \
    -o with_tests=False

ABS_DIST_PATH="$(pwd)/$DIST_DIR"
TOOLCHAIN_ARGS=()
if [ -f "$BUILD_DIR/conan_toolchain.cmake" ]; then
    TOOLCHAIN_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake")
fi

GENERATOR_ARGS=()
if command -v ninja &> /dev/null; then
    GENERATOR_ARGS=(-G Ninja)
fi

echo -e "${BLUE}>>> [3/5] Configuring CMake build...${NC}"
cmake -S . -B "$BUILD_DIR" "${GENERATOR_ARGS[@]}" "${TOOLCHAIN_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DZYX_BUILD_TESTS=OFF \
    -DZYX_BUILD_APPS=ON \
    -DZYX_BUILD_PYTHON=OFF \
    -DCMAKE_INSTALL_PREFIX="$ABS_DIST_PATH" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON

echo -e "${BLUE}>>> [4/5] Compiling and installing...${NC}"
cmake --build "$BUILD_DIR" --config Release
cmake --install "$BUILD_DIR" --config Release

if [ ! -f "$DIST_DIR/include/zyx/zyx.hpp" ]; then
    echo -e "${RED}Error: Install failed. Header files missing.${NC}"
    exit 1
fi

if [[ "$OSTYPE" == "darwin"* ]]; then
    LIB_PATH="$DIST_DIR/lib/libzyx.dylib"
    if [ -f "$LIB_PATH" ]; then
        otool -D "$LIB_PATH"
    else
        echo -e "${YELLOW}Warning: Library not found at $LIB_PATH for dylib verification.${NC}"
    fi
fi

echo -e "${GREEN}SDK installed to: $DIST_DIR${NC}"

echo -e "${BLUE}>>> [5/5] Packaging artifact...${NC}"
TARBALL_NAME="${FULL_PACKAGE_NAME}.tar.gz"
tar -czf "$BUILD_DIR/$TARBALL_NAME" -C "$DIST_DIR" .

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} Release Build Successful! ${NC}"
echo -e "${GREEN} Artifact: $BUILD_DIR/$TARBALL_NAME ${NC}"
echo -e "${GREEN} Location: $(pwd)/$BUILD_DIR/$TARBALL_NAME ${NC}"
echo -e "${GREEN}======================================================${NC}"

ls -lh "$BUILD_DIR/$TARBALL_NAME"
