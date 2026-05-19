#!/bin/bash

# ==============================================================================
# Script Name: run_tests.sh
# Purpose: Clean, configure, build, test, and generate coverage reports.
# Usage: ./run_tests.sh [--quick] [--no-conan] [--html] [--file <filename>]
# ==============================================================================

set -e

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

BUILD_DIR="buildDir"
COVERAGE_ROOT="$BUILD_DIR/coverage"
PROFILES_DIR="$COVERAGE_ROOT/raw"
PYTHON_SCRIPT="scripts/generate_coverage.py"

QUICK_MODE=false
NO_CONAN=false
REPORT_ARGS=""

while [[ $# -gt 0 ]]; do
  case $1 in
    -q|--quick)
      QUICK_MODE=true
      shift
      ;;
    --no-conan)
      NO_CONAN=true
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

if [ "$QUICK_MODE" = true ] && [ -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}>>> [1/5] Quick Mode: Reusing existing build directory.${NC}"
    rm -rf "$PROFILES_DIR"
    mkdir -p "$PROFILES_DIR"
else
    echo -e "${BLUE}>>> [1/5] Cleaning and installing dependencies...${NC}"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR" "$PROFILES_DIR"

    if [ "$NO_CONAN" != true ] && command -v conan &> /dev/null; then
        conan profile detect 2>/dev/null || true
        conan install . \
            --output-folder="$BUILD_DIR" \
            --build=missing \
            --build="antlr4-cppruntime/*" \
            -s build_type=Debug \
            -s compiler.cppstd=20
    fi
fi

mkdir -p "$PROFILES_DIR"
PROFILE_PATH="$(pwd)/$PROFILES_DIR/code-%p.profraw"
export LLVM_PROFILE_FILE="$PROFILE_PATH"

TOOLCHAIN_ARGS=()
if [ -f "$BUILD_DIR/conan_toolchain.cmake" ]; then
    TOOLCHAIN_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake")
fi

GENERATOR_ARGS=()
if command -v ninja &> /dev/null; then
    GENERATOR_ARGS=(-G Ninja)
fi

echo -e "${BLUE}>>> [2/5] Configuring and compiling...${NC}"
cmake -S . -B "$BUILD_DIR" "${GENERATOR_ARGS[@]}" "${TOOLCHAIN_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DZYX_BUILD_TESTS=ON \
    -DZYX_BUILD_APPS=ON \
    -DZYX_ENABLE_COVERAGE=ON

cmake --build "$BUILD_DIR" --target zyx_test_suite

echo -e "${BLUE}>>> [3/5] Running tests...${NC}"
"./$BUILD_DIR/zyx_test_suite" < /dev/null

echo -e "${BLUE}>>> [4/5] Verifying CTest registration...${NC}"
ctest --test-dir "$BUILD_DIR" -N

echo -e "${BLUE}>>> [5/5] Generating reports...${NC}"
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

python3 "$PYTHON_SCRIPT" "$BUILD_DIR" \
    --llvm-cov "$LLVM_COV_CMD" \
    --llvm-prof "$LLVM_PROF_CMD" \
    $REPORT_ARGS

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} Done! ${NC}"
