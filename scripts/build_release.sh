#!/bin/bash

# ==============================================================================
# Script Name: build_release.sh
# Purpose: Build optimized shared libraries and generate the SDK artifact.
# Output: build/release/dist (folder) and build/zyx-sdk-<ver>-<os>.tar.gz
# ==============================================================================

set -e # Exit immediately on error

# Define colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# ==============================================================================
# Configuration
# ==============================================================================
BUILD_DIR="build_release"
DIST_DIR="$BUILD_DIR/dist"      # This will look like /usr/local/ but inside our folder
ARTIFACT_NAME="zyx-sdk"
VERSION="0.1.0"                 # You could grab this from meson.build using grep if needed

# Detect OS for naming
if [[ "$OSTYPE" == "darwin"* ]]; then
    OS_TAG="macos"
else
    OS_TAG="linux"
fi

# Get Architecture (arm64 or x86_64)
ARCH_TAG=$(uname -m)

FULL_PACKAGE_NAME="${ARTIFACT_NAME}-${VERSION}-${OS_TAG}-${ARCH_TAG}"

# ==============================================================================
# Step 1: Clean and Prepare
# ==============================================================================
echo -e "${BLUE}>>> [1/5] Cleaning release environment...${NC}"

if [ -d "$BUILD_DIR" ]; then
    echo "Removing old build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

# ==============================================================================
# Step 2: Install Dependencies (Release Mode)
# ==============================================================================
echo -e "${BLUE}>>> [2/5] Installing Dependencies (Release Mode)...${NC}"

# Note: We use build_type=Release here for maximum performance (O3 optimization)
conan install . \
    --output-folder="$BUILD_DIR" \
    --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=20 \
    -o *:shared=False \
    -o *:fPIC=True

# ==============================================================================
# Step 3: Configure Build (Meson)
# ==============================================================================
echo -e "${BLUE}>>> [3/5] Configuring Build System...${NC}"

# --prefix: Tells meson where to 'install' the files. We use a local absolute path.
# --libdir: Ensure libraries go to 'lib' folder, not 'lib/x86_64-linux-gnu' etc.
# -Dbuildtype=release: Enables optimizations, disables assertions.
# -Db_lto=true: Enables Link Time Optimization (optional, makes binaries smaller/faster).

ABS_DIST_PATH="$(pwd)/$DIST_DIR"

meson setup "$BUILD_DIR" \
    --native-file "$BUILD_DIR/conan_meson_native.ini" \
    --prefix="$ABS_DIST_PATH" \
    --libdir=lib \
    -Dbuildtype=release \
    -Ddefault_library=shared \
    -Db_lto=true \
    -Dwarning_level=0

# ==============================================================================
# Step 4: Compile and Install
# ==============================================================================
echo -e "${BLUE}>>> [4/5] Compiling and Installing...${NC}"

# Compile
meson compile -C "$BUILD_DIR"

# Install (This copies headers and libs to $DIST_DIR)
meson install -C "$BUILD_DIR"

# macOS Post-Install Fixup
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "Performing macOS dylib fixup..."

    # Define the library path inside the dist folder
    # Note: Assuming your library is named 'libzyx.dylib' based on your output.
    # If it is 'libzyx.dylib', please adjust the name below.
    LIB_PATH="$DIST_DIR/lib/libzyx.dylib"

    if [ -f "$LIB_PATH" ]; then
        # Force the ID to be @rpath/libzyx.dylib
        # This makes the library relocatable (can be moved to any folder)
        install_name_tool -id "@rpath/libzyx.dylib" "$LIB_PATH"

        # Check if successful
        otool -D "$LIB_PATH"
    else
        echo -e "${RED}Warning: Library not found at $LIB_PATH for fixup.${NC}"
    fi
fi

# Verify installation
if [ ! -f "$DIST_DIR/include/zyx/zyx.hpp" ]; then
    echo -e "${RED}Error: Install failed. Header files missing.${NC}"
    exit 1
fi

echo -e "${GREEN}SDK installed to: $DIST_DIR${NC}"

# ==============================================================================
# Step 5: Package Artifact
# ==============================================================================
echo -e "${BLUE}>>> [5/5] Packaging Artifact...${NC}"

TARBALL_NAME="${FULL_PACKAGE_NAME}.tar.gz"

# Create a tarball of the dist folder
# -C changes directory so the tarball starts at the root of the SDK
tar -czf "$BUILD_DIR/$TARBALL_NAME" -C "$DIST_DIR" .

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} Release Build Successful! ${NC}"
echo -e "${GREEN} Artifact: $BUILD_DIR/$TARBALL_NAME ${NC}"
echo -e "${GREEN} Location: $(pwd)/$BUILD_DIR/$TARBALL_NAME ${NC}"
echo -e "${GREEN}======================================================${NC}"

# Optional: List content size
ls -lh "$BUILD_DIR/$TARBALL_NAME"