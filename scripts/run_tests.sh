#!/bin/bash

# ==============================================================================
# Script Name: run_tests.sh
# Purpose: Clean, configure, build, test, and generate coverage reports.
# Usage: ./run_tests.sh [--quick] [--html] [--file <filename>]
# ==============================================================================

set -e # Exit immediately on error

# Define colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Configuration
BUILD_DIR="buildDir"
COVERAGE_ROOT="$BUILD_DIR/coverage"
PROFILES_DIR="$COVERAGE_ROOT/raw"
PYTHON_SCRIPT="scripts/generate_coverage.py"

# Set profile path for LLVM
export LLVM_PROFILE_FILE="$(pwd)/$PROFILES_DIR/code-%p.profraw"

# Argument Parsing
QUICK_MODE=false
REPORT_ARGS=""

while [[ $# -gt 0 ]]; do
  case $1 in
    -q|--quick)
      QUICK_MODE=true
      shift
      ;;
    --html)
      REPORT_ARGS+=" --html"
      shift
      ;;
    --file)
      REPORT_ARGS+=" --file $2"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1"
      exit 1
      ;;
  esac
done

# ==============================================================================
# Step 1: Environment Setup
# ==============================================================================
if [ "$QUICK_MODE" = true ] && [ -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}>>> [1/5] Quick Mode: Skipping full install.${NC}"
    rm -rf "$PROFILES_DIR" # Clear old profiles
    mkdir -p "$PROFILES_DIR"
else
    echo -e "${BLUE}>>> [1/5] Cleaning and Installing...${NC}"
    if [ -d "$BUILD_DIR" ]; then rm -rf "$BUILD_DIR"; fi
    mkdir -p "$BUILD_DIR" "$PROFILES_DIR"

    conan profile detect 2>/dev/null || true
    conan install . --output-folder="$BUILD_DIR" --build=missing -s build_type=Debug -s compiler.cppstd=20
fi

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
meson test -C "$BUILD_DIR"

# ==============================================================================
# Step 4: Generate Reports
# ==============================================================================
echo -e "${BLUE}>>> [5/5] Generating Reports...${NC}"

if [ ! -f "$PYTHON_SCRIPT" ]; then
    echo -e "${RED}Error: $PYTHON_SCRIPT not found.${NC}"
    exit 1
fi

if [[ "$OSTYPE" == "darwin"* ]]; then
    LLVM_COV_CMD=$(xcrun -f llvm-cov)
    LLVM_PROF_CMD=$(xcrun -f llvm-profdata)
else
    LLVM_COV_CMD="llvm-cov"
    LLVM_PROF_CMD="llvm-profdata"
fi

# FIX: Passed tools as named arguments instead of positionals to prevent misalignment
python3 "$PYTHON_SCRIPT" "$BUILD_DIR" \
    --llvm-cov "$LLVM_COV_CMD" \
    --llvm-prof "$LLVM_PROF_CMD" \
    $REPORT_ARGS

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} Done! ${NC}"