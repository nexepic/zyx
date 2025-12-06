#!/bin/bash

# ==============================================================================
# Script Name: run_tests.sh
# Purpose: Clean, configure, build, test, and generate HTML & LCOV coverage.
# ==============================================================================

set -e # Exit immediately on error

# Define colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# ==============================================================================
# Configuration & Paths
# ==============================================================================
BUILD_DIR="buildDir"
# All coverage artifacts go here
COVERAGE_ROOT="$BUILD_DIR/coverage"
PROFILES_DIR="$COVERAGE_ROOT/raw"
HTML_REPORT_DIR="$COVERAGE_ROOT/html"
LCOV_FILE="$COVERAGE_ROOT/coverage.lcov"

PYTHON_SCRIPT="scripts/generate_coverage.py"

# Set the profile path GLOBALLY at the start.
# This prevents 'meson setup' compiler checks from writing .profraw files to the root.
# We use $(pwd) to ensure absolute paths.
export LLVM_PROFILE_FILE="$(pwd)/$PROFILES_DIR/code-%p.profraw"

# Argument Parsing (Quick Mode)
QUICK_MODE=false
for arg in "$@"; do
  case $arg in
    -q|--quick) QUICK_MODE=true; shift ;;
  esac
done

# ==============================================================================
# Step 1: Environment Setup
# ==============================================================================
if [ "$QUICK_MODE" = true ] && [ -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}>>> [1/5] Quick Mode: Skipping full install.${NC}"

    # Even in quick mode, we must ensure the profile directory exists and is clean
    # so we don't merge old test runs with new ones.
    rm -rf "$PROFILES_DIR"
    mkdir -p "$PROFILES_DIR"
else
    echo -e "${BLUE}>>> [1/5] Cleaning and Installing...${NC}"

    # Clean build directory
    if [ -d "$BUILD_DIR" ]; then rm -rf "$BUILD_DIR"; fi

    # Create directories immediately so LLVM_PROFILE_FILE path is valid
    mkdir -p "$BUILD_DIR"
    mkdir -p "$PROFILES_DIR"

    # Install dependencies
    conan profile detect 2>/dev/null || true
    conan install . --output-folder="$BUILD_DIR" --build=missing -s build_type=Debug -s compiler.cppstd=20
fi

# Ensure coverage root exists (double check)
mkdir -p "$PROFILES_DIR"

# ==============================================================================
# Step 2: Configure & Compile
# ==============================================================================
echo -e "${BLUE}>>> [2/5] Configuring & Compiling...${NC}"

export FLAGS="-fprofile-instr-generate -fcoverage-mapping -std=c++20"

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    meson setup "$BUILD_DIR" --native-file "$BUILD_DIR/conan_meson_native.ini" \
      -Dcpp_args="$FLAGS" -Dcpp_link_args="$FLAGS" -Dbuildtype=debug
else
    meson setup "$BUILD_DIR" --reconfigure -Dcpp_args="$FLAGS" -Dcpp_link_args="$FLAGS"
fi

meson compile -C "$BUILD_DIR"

# ==============================================================================
# Step 3: Run Tests
# ==============================================================================
echo -e "${BLUE}>>> [4/5] Running Tests...${NC}"

# Note: LLVM_PROFILE_FILE is already exported at the top of the script.
meson test -C "$BUILD_DIR"

# ==============================================================================
# Step 4: Generate Reports
# ==============================================================================
echo -e "${BLUE}>>> [5/5] Generating Reports...${NC}"

if [ ! -f "$PYTHON_SCRIPT" ]; then
    echo -e "${RED}Error: $PYTHON_SCRIPT not found.${NC}"
    exit 1
fi

# Resolve tools path
if [[ "$OSTYPE" == "darwin"* ]]; then
    LLVM_COV_CMD=$(xcrun -f llvm-cov)
    LLVM_PROF_CMD=$(xcrun -f llvm-profdata)
else
    LLVM_COV_CMD="llvm-cov"
    LLVM_PROF_CMD="llvm-profdata"
fi

# 1. Generate LCOV (Good for parsing/CI)
echo "Generating LCOV..."
python3 "$PYTHON_SCRIPT" "$BUILD_DIR" "$LCOV_FILE" "$LLVM_COV_CMD" "$LLVM_PROF_CMD"

# 2. Generate HTML (Good for humans)
echo "Generating HTML..."
python3 "$PYTHON_SCRIPT" "$BUILD_DIR" "$HTML_REPORT_DIR" "$LLVM_COV_CMD" "$LLVM_PROF_CMD"

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} Success! Reports generated inside '$COVERAGE_ROOT' ${NC}"
echo -e "${GREEN}======================================================${NC}"

# Open HTML report on macOS
if [[ "$OSTYPE" == "darwin"* ]]; then
    open "$HTML_REPORT_DIR/index.html"
fi