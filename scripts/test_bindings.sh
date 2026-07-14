#!/usr/bin/env bash
# ==============================================================================
# Script Name: test_bindings.sh
# Purpose: Build (from current source) and run tests for each language binding
#          (Node.js / Python / Rust), mirroring their GitHub Actions workflows.
#
# Why this exists: the per-binding local entry points (npm test, pytest, cargo
# test) do NOT rebuild the native library — they reuse whatever binary is on
# disk. Running them after a `git pull` therefore tests a STALE binary (this
# caused the "Insert failed: Decompression failed" incident where a 5/24-built
# zyxdb.node was tested against 7月 源码). This script forces "build then test"
# so bindings always run against the current source.
#
# Usage:
#   scripts/test_bindings.sh              # build + test all bindings
#   scripts/test_bindings.sh --node       # Node.js only
#   scripts/test_bindings.sh --python     # Python only
#   scripts/test_bindings.sh --rust       # Rust only
#   scripts/test_bindings.sh --skip-conan # reuse an existing buildDir/conan_toolchain.cmake
#   PY_BIN=python3.12 ./scripts/test_bindings.sh --python   # use a specific Python (default: python3.14)
#
# Prerequisites: conan, ninja, cmake, node/npm, python/pip, cargo (only the
# toolchains for the bindings you select are required).
# ==============================================================================
set -euo pipefail

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/buildDir"
TOOLCHAIN="${BUILD_DIR}/conan_toolchain.cmake"
BUILD_TYPE="Release"

DO_NODE=0
DO_PYTHON=0
DO_RUST=0
DO_CONAN=1

# Python interpreter used to build/test the Python binding. Default to 3.14 so
# the editable (PEP 660) install works even on machines whose system python3 is
# too old (macOS CommandLineTools ships 3.9 / pip 21.2, which cannot do PEP 660
# editable installs with scikit-build-core). Override with: PY_BIN=python3.12 ...
PYTHON_BIN="${PY_BIN:-python3.14}"
PYTHON_MIN_MAJOR=3
PYTHON_MIN_MINOR=10
PIP_MIN_MAJOR=21
PIP_MIN_MINOR=3

# Parse args -------------------------------------------------------------------
if [[ $# -eq 0 ]]; then
	DO_NODE=1; DO_PYTHON=1; DO_RUST=1
fi
while [[ $# -gt 0 ]]; do
	case "$1" in
		--node)   DO_NODE=1; shift ;;
		--python) DO_PYTHON=1; shift ;;
		--rust)   DO_RUST=1; shift ;;
		--all)    DO_NODE=1; DO_PYTHON=1; DO_RUST=1; shift ;;
		--skip-conan) DO_CONAN=0; shift ;;
		--python-bin) PYTHON_BIN="$2"; shift 2 ;;
		--build-type) BUILD_TYPE="$2"; shift 2 ;;
		-h|--help)
			sed -n '2,30p' "${BASH_SOURCE[0]:-$0}"
			exit 0 ;;
		*)
			echo -e "${RED}Unknown argument: $1${NC}" >&2
			exit 2 ;;
	esac
done

log() { echo -e "${BLUE}>>> $*${NC}"; }
ok()  { echo -e "${GREEN}    [OK] $*${NC}"; }
warn(){ echo -e "${YELLOW}>>> [WARN] $*${NC}"; }
die() { echo -e "${RED}>>> [FAIL] $*${NC}" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# Mirror CI: detect Linux clang-20 toolchain, otherwise fall back to defaults.
linux_clang_env() {
	if [[ "$(uname -s)" == "Linux" ]] && have clang-20; then
		export CC=clang-20
		export CXX=clang++-20
	fi
}

# Conan install ----------------------------------------------------------------
ensure_conan_toolchain() {
	if [[ "${DO_CONAN}" -eq 0 ]]; then
		[[ -f "${TOOLCHAIN}" ]] || die "--skip-conan set but ${TOOLCHAIN} not found; run once without --skip-conan."
		log "Reusing existing Conan toolchain: ${TOOLCHAIN}"
		return
	fi
	have conan || die "conan not found (pip install conan)"
	[[ -f "${HOME}/.conan2/profiles/default" ]] || conan profile detect
	mkdir -p "${BUILD_DIR}"
	log "Installing Conan dependencies (build_type=${BUILD_TYPE}) into ${BUILD_DIR}"
	# Keep flags identical to CI: Release, C++20, antlr4 built from source, no tests.
	conan install "${ROOT_DIR}" \
		--output-folder="${BUILD_DIR}" \
		--build=missing \
		--build="antlr4-cppruntime/*" \
		-s "build_type=${BUILD_TYPE}" \
		-s compiler.cppstd=20 \
		-o with_tests=False
	ok "Conan toolchain ready"
}

# Node.js ----------------------------------------------------------------------
test_node() {
	local dir="${ROOT_DIR}/bindings/nodejs"
	linux_clang_env
	log "Installing npm deps (no install scripts)"
	( cd "${dir}" && npm install --ignore-scripts )
	log "Building native addon (cmake-js compile)"
	( cd "${dir}" && npx cmake-js compile --CDCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" )
	log "Running npm test:nobuild (addon already built above)"
	# package.json's `test` also runs `npm run build`; use test:nobuild to avoid
	# a redundant rebuild — the addon was just compiled from current source.
	( cd "${dir}" && npm run test:nobuild )
	ok "Node.js binding tests passed"
}

# Python -----------------------------------------------------------------------
check_python_env() {
	# Fail fast with a clear message instead of letting pip surface a confusing
	# "setup.py not found" error. scikit-build-core's editable (PEP 660) install
	# needs Python >= 3.10 and pip >= 21.3.
	command -v "${PYTHON_BIN}" >/dev/null 2>&1 || {
		die "Python binding: '${PYTHON_BIN}' not found. Set PY_BIN (e.g. PY_BIN=python3.12 ./scripts/test_bindings.sh --python) or install Python >= ${PYTHON_MIN_MAJOR}.${PYTHON_MIN_MINOR}."
	}
	local py_ver pip_ver pip_major pip_minor
	py_ver="$("${PYTHON_BIN}" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
	if [[ "${py_ver}" < "${PYTHON_MIN_MAJOR}.${PYTHON_MIN_MINOR}" ]]; then
		die "Python binding: ${PYTHON_BIN} is ${py_ver}, but project requires >= ${PYTHON_MIN_MAJOR}.${PYTHON_MIN_MINOR} (bindings/python/pyproject.toml requires-python). Override with PY_BIN."
	fi
	pip_ver="$("${PYTHON_BIN}" -m pip --version 2>/dev/null | awk '{print $2}')" || die "${PYTHON_BIN} -m pip unavailable"
	pip_major="${pip_ver%%.*}"; pip_minor="${pip_ver#*.}"; pip_minor="${pip_minor%%.*}"
	if (( pip_major < PIP_MIN_MAJOR )) || { (( pip_major == PIP_MIN_MAJOR )) && (( pip_minor < PIP_MIN_MINOR )); }; then
		die "Python binding: ${PYTHON_BIN}'s pip is ${pip_ver}, but editable (PEP 660) installs need >= ${PIP_MIN_MAJOR}.${PIP_MIN_MINOR}. Run: ${PYTHON_BIN} -m pip install -U pip"
	fi
}

test_python() {
	local dir="${ROOT_DIR}/bindings/python"
	linux_clang_env
	check_python_env
	log "Using Python: $(${PYTHON_BIN} --version) ($(${PYTHON_BIN} -m pip --version | awk '{print $2}'))"
	if [[ -z "${VIRTUAL_ENV:-}" ]]; then
		warn "Not in a virtualenv; installing into the active Python environment (${PYTHON_BIN})."
	fi
	log "Building + installing zyxdb (editable, [test])"
	CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} -DCMAKE_BUILD_TYPE=${BUILD_TYPE}" \
		"${PYTHON_BIN}" -m pip install -e "${dir}[test]"
	log "Running pytest"
	( cd "${dir}" && "${PYTHON_BIN}" -m pytest tests/ -v )
	ok "Python binding tests passed"
}

# Rust -------------------------------------------------------------------------
test_rust() {
	local dir="${ROOT_DIR}/bindings/rust"
	linux_clang_env
	# Use `system` feature against a prebuilt libzyx, exactly like CI: build the
	# shared library once, point ZYX_LIB_DIR at it, then cargo test.
	log "Building native Driver ABI library (target zyx)"
	cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
		-DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
		-DZYX_BUILD_TESTS=OFF \
		-DZYX_BUILD_APPS=OFF \
		-DZYX_BUILD_PYTHON=OFF
	cmake --build "${BUILD_DIR}" --target zyx
	log "Running cargo test (system feature)"
	ZYX_LIB_DIR="${BUILD_DIR}" \
		cargo test --manifest-path "${dir}/Cargo.toml" --workspace --no-default-features --features system
	ok "Rust binding tests passed"
}

# Run a binding only if its toolchain is present; warn (don't fail) if missing.
run_guarded() { # name selector fn
	local name="$1"; local selector="$2"; local fn="$3"
	if [[ "${selector}" -ne 1 ]]; then
		return 0
	fi
	case "${name}" in
		Node.js) have node && have npm  || { warn "${name}: node/npm missing — skipped"; return 0; } ;;
		Python)  command -v "${PYTHON_BIN}" >/dev/null 2>&1 && "${PYTHON_BIN}" -m pip --version >/dev/null 2>&1 || { warn "${name}: '${PYTHON_BIN}'/pip missing — skipped (override with PY_BIN)"; return 0; } ;;
		Rust)    have cargo || { warn "${name}: cargo missing — skipped"; return 0; } ;;
	esac
	"${fn}"
}

# Main -------------------------------------------------------------------------
ensure_conan_toolchain

run_guarded "Node.js" "${DO_NODE}"   test_node
run_guarded "Python"  "${DO_PYTHON}" test_python
run_guarded "Rust"    "${DO_RUST}"   test_rust

echo -e "${GREEN}=== All requested binding tests passed ===${NC}"