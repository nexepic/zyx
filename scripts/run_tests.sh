#!/bin/bash

# ==============================================================================
# Script Name: run_tests.sh
# Purpose: Clean, configure, build, test, and generate coverage reports.
# Usage: ./run_tests.sh [--quick] [--no-conan] [--html] [--file <filename>] [--gtest-filter <pattern>]
# ==============================================================================

set -euo pipefail

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

BUILD_DIR="buildDir"
COVERAGE_ROOT="$BUILD_DIR/coverage"
TEST_RESULT_FILE="$COVERAGE_ROOT/test-shards-result.json"
PYTHON_SCRIPT="scripts/generate_coverage.py"
TEST_RUNNER="scripts/run_test_process.py"
SHARD_RUNNER="scripts/run_test_shards.py"
# Native durability tests intentionally synchronize single-file data and metadata on every
# isolated database lifecycle. Ten minutes is insufficient on slower local,
# virtualized, or Windows filesystems; callers can still lower this explicitly.
TEST_TIMEOUT_SECONDS="${ZYX_TEST_TIMEOUT_SECONDS:-1800}"
TEST_SHARDS="${ZYX_TEST_SHARDS:-}"
# Keep each invocation's profiles separate so a report can be tied to exactly
# one test execution rather than accidentally merging a stale background run.
TEST_RUN_ID="${ZYX_TEST_RUN_ID:-native-$(date +%Y%m%dT%H%M%S)-$$}"
KEEP_COVERAGE_ARTIFACTS="${ZYX_TEST_KEEP_COVERAGE_ARTIFACTS:-false}"

# The run id becomes part of paths later removed by cleanup; keep it to one
# directory name even when a caller supplies it explicitly.
if ! [[ "$TEST_RUN_ID" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
    echo -e "${RED}Error: ZYX_TEST_RUN_ID may contain only letters, digits, '.', '_', and '-'.${NC}" >&2
    exit 1
fi
PROFILES_DIR="$COVERAGE_ROOT/runs/$TEST_RUN_ID/raw"
FAILED_SHARD_LOG_DIR="$COVERAGE_ROOT/runs/$TEST_RUN_ID/failed-shards"

if [ ! -f "$PYTHON_SCRIPT" ]; then
    echo -e "${RED}Error: $PYTHON_SCRIPT not found.${NC}"
    exit 1
fi
if [ ! -f "$TEST_RUNNER" ]; then
    echo -e "${RED}Error: $TEST_RUNNER not found.${NC}"
    exit 1
fi
if [ ! -f "$SHARD_RUNNER" ]; then
    echo -e "${RED}Error: $SHARD_RUNNER not found.${NC}"
    exit 1
fi

# A lock prevents concurrent runs from sharing build, database, and profraw
# files.  The directory is intentionally outside the repository so a failed
# run cannot leave tracked or generated files behind.
LOCK_DIR="${TMPDIR:-/tmp}/zyx-run-tests.lock"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  lock_pid=""
  if [ -f "$LOCK_DIR/pid" ]; then
    lock_pid=$(<"$LOCK_DIR/pid")
  fi
  if [ -n "$lock_pid" ] && kill -0 "$lock_pid" 2>/dev/null; then
    echo -e "${RED}Another ZYX test run is already active (PID $lock_pid).${NC}" >&2
    exit 1
  fi
  # A SIGKILL can leave the marker behind.  It is safe to reclaim it when the
  # recorded owner no longer exists.
  rm -rf "$LOCK_DIR"
  mkdir "$LOCK_DIR"
fi
printf '%s\n' "$$" > "$LOCK_DIR/pid"

# A forced termination bypasses the EXIT trap and can leave a prior run's raw
# profiles and shard logs behind. Ordinary runs never need those artifacts;
# an explicit diagnostic run opts out of this reclamation.
if [ "$KEEP_COVERAGE_ARTIFACTS" != "true" ]; then
  rm -rf "$COVERAGE_ROOT/runs" 2>/dev/null || true
fi

cleanup() {
  local status=$?
  # Raw profiles and shard logs are input to this invocation's report only.
  # Keep them only when an explicit diagnostic investigation needs them.
  if [ "$KEEP_COVERAGE_ARTIFACTS" != "true" ]; then
    rm -rf "$COVERAGE_ROOT/runs/$TEST_RUN_ID" 2>/dev/null || true
    # The runner result is printed to stdout and is not needed after the report.
    rm -f "$TEST_RESULT_FILE" 2>/dev/null || true
    # The merged profile is only an intermediate report input; it is regenerated
    # on the next run and should not accumulate in the build directory.
    rm -f "$BUILD_DIR/code.profdata" 2>/dev/null || true
    # A direct test invocation can leave LLVM's default fallback profile here.
    # It carries no report state and is always recreated when needed.
    rm -f "$BUILD_DIR/default.profraw" 2>/dev/null || true
    # Some direct test invocations write LLVM's fallback profile to the
    # repository root. It is likewise disposable coverage intermediate data.
    rm -f "default.profraw" 2>/dev/null || true
    # This invocation writes all raw profiles below its run directory. Avoid a
    # full build-cache scan here so routine focused tests remain fast.
    find . -maxdepth 1 -type f -name '*.profraw' -delete 2>/dev/null || true
    # Remove only empty parents, preserving an explicitly requested HTML report.
    rmdir "$COVERAGE_ROOT/runs" "$COVERAGE_ROOT" 2>/dev/null || true
  fi
  # CTest's last-run log is diagnostic output, not coverage state.
  rm -f "$BUILD_DIR/Testing/Temporary/LastTest.log" 2>/dev/null || true
  rm -rf "$LOCK_DIR" 2>/dev/null || true
  exit "$status"
}
trap cleanup EXIT INT TERM

QUICK_MODE=false
NO_CONAN=false
REPORT_ARGS=()
TEST_ARGS=()
GTEST_FILTER=""

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
      REPORT_ARGS+=(--html)
      shift
      ;;
    --file)
      if [ "$#" -lt 2 ]; then
        echo "Error: --file requires a source-file path"
        exit 1
      fi
      REPORT_ARGS+=(--file "$2")
      shift 2
      ;;
    --gtest-filter)
      if [ "$#" -lt 2 ] || [ -z "$2" ]; then
        echo "Error: --gtest-filter requires a GoogleTest filter pattern"
        exit 1
      fi
      if [ -n "$GTEST_FILTER" ]; then
        echo "Error: --gtest-filter may be specified only once"
        exit 1
      fi
      GTEST_FILTER="$2"
      shift 2
      ;;
    --gtest-filter=*)
      FILTER_VALUE="${1#--gtest-filter=}"
      if [ -z "$FILTER_VALUE" ]; then
        echo "Error: --gtest-filter requires a GoogleTest filter pattern"
        exit 1
      fi
      if [ -n "$GTEST_FILTER" ]; then
        echo "Error: --gtest-filter may be specified only once"
        exit 1
      fi
      GTEST_FILTER="$FILTER_VALUE"
      shift
      ;;
    *)
      echo "Unknown argument: $1"
      exit 1
      ;;
  esac
done

if [ -z "$TEST_SHARDS" ]; then
    # A filtered run is an iterative, per-module coverage check. One process
    # avoids shard setup overhead and leaves one unambiguous profile to inspect.
    if [ -n "$GTEST_FILTER" ]; then
        TEST_SHARDS=1
    else
        case "$OSTYPE" in
            darwin*|linux*) TEST_SHARDS=2 ;;
            *) TEST_SHARDS=1 ;;
        esac
    fi
fi
if ! [[ "$TEST_SHARDS" =~ ^[1-9][0-9]*$ ]]; then
    echo -e "${RED}Error: ZYX_TEST_SHARDS must be a positive integer.${NC}"
    exit 1
fi

if [ -n "$GTEST_FILTER" ]; then
    TEST_ARGS=("--gtest_filter=$GTEST_FILTER")
    echo -e "${BLUE}>>> GoogleTest filter: $GTEST_FILTER${NC}"
else
    if [ "${#REPORT_ARGS[@]}" -gt 0 ]; then
        echo -e "${YELLOW}>>> Coverage detail does not limit tests; add --gtest-filter with exact test names for an iterative file check.${NC}"
    fi
    # Keep a complete test run's output bounded in CI and terminal harnesses.
    # Failed suites and the final GoogleTest summary remain visible.
    export ZYX_TEST_TERSE_OUTPUT=1
    # Long-running suites intentionally stay quiet in terse mode. Emit a
    # low-frequency supervisor heartbeat so noninteractive sessions do not
    # mistake healthy test execution for a stalled command.
    export ZYX_TEST_HEARTBEAT_SECONDS="${ZYX_TEST_HEARTBEAT_SECONDS:-10}"
fi

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
rm -f "$TEST_RESULT_FILE"
rm -rf "$FAILED_SHARD_LOG_DIR"
PROFILE_PATH="$(pwd)/$PROFILES_DIR/code-%p.profraw"
export LLVM_PROFILE_FILE="$PROFILE_PATH"

CONFIGURE_ARGS=(-DCMAKE_BUILD_TYPE=Debug -DZYX_BUILD_TESTS=ON -DZYX_BUILD_APPS=ON -DZYX_ENABLE_COVERAGE=ON)
if [ -f "$BUILD_DIR/conan_toolchain.cmake" ]; then
    CONFIGURE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake")
fi
if command -v ninja &> /dev/null; then
    CONFIGURE_ARGS+=(-G Ninja)
fi

echo -e "${BLUE}>>> [2/5] Configuring and compiling...${NC}"
cmake -S . -B "$BUILD_DIR" "${CONFIGURE_ARGS[@]}"

cmake --build "$BUILD_DIR" --target zyx_test_suite

echo -e "${BLUE}>>> [3/5] Running tests...${NC}"
if [ -n "$GTEST_FILTER" ]; then
    python3 "$SHARD_RUNNER" --jobs "$TEST_SHARDS" --timeout "$TEST_TIMEOUT_SECONDS" \
        --result-file "$TEST_RESULT_FILE" --run-id "$TEST_RUN_ID" \
        --failure-log-dir "$FAILED_SHARD_LOG_DIR" -- \
        "$(pwd)/$BUILD_DIR/zyx_test_suite" "${TEST_ARGS[@]}"
else
    python3 "$SHARD_RUNNER" --jobs "$TEST_SHARDS" --timeout "$TEST_TIMEOUT_SECONDS" \
        --result-file "$TEST_RESULT_FILE" --run-id "$TEST_RUN_ID" \
        --failure-log-dir "$FAILED_SHARD_LOG_DIR" -- \
        "$(pwd)/$BUILD_DIR/zyx_test_suite"
fi
cat "$TEST_RESULT_FILE"

echo -e "${BLUE}>>> [4/5] Verifying CTest registration...${NC}"
ctest --test-dir "$BUILD_DIR" -N

echo -e "${BLUE}>>> [5/5] Generating reports...${NC}"
if [[ "$OSTYPE" == "darwin"* ]]; then
    LLVM_COV_CMD=$(xcrun -f llvm-cov)
    LLVM_PROF_CMD=$(xcrun -f llvm-profdata)
else
    LLVM_COV_CMD="llvm-cov"
    LLVM_PROF_CMD="llvm-profdata"
fi

if [ "${#REPORT_ARGS[@]}" -gt 0 ]; then
    python3 "$PYTHON_SCRIPT" "$BUILD_DIR" \
        --llvm-cov "$LLVM_COV_CMD" \
        --llvm-prof "$LLVM_PROF_CMD" \
        --profile-dir "$PROFILES_DIR" \
        "${REPORT_ARGS[@]}"
else
    python3 "$PYTHON_SCRIPT" "$BUILD_DIR" \
        --llvm-cov "$LLVM_COV_CMD" \
        --llvm-prof "$LLVM_PROF_CMD" \
        --profile-dir "$PROFILES_DIR"
fi

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN} Done! ${NC}"
