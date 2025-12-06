# Metrix

[![Build Status](https://github.com/nexepic/metrix/actions/workflows/build.yml/badge.svg)](https://github.com/nexepic/metrix/actions/workflows/build.yml)
[![codecov](https://codecov.io/github/nexepic/metrix/graph/badge.svg?token=VBCGBBI4YO)](https://codecov.io/github/nexepic/metrix)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C++-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Meson](https://img.shields.io/badge/Build-Meson-blue.svg)](https://mesonbuild.com/)

A high-performance, embeddable graph database engine written in modern C++20, designed for efficient storage, indexing, and querying of graph-structured data.

## Features

### Core Capabilities
- **Native Graph Storage**: Purpose-built data structures for nodes, edges, and properties with efficient serialization.
- **ACID Transactions**: Full transaction support with rollback capabilities and Write-Ahead Logging (WAL).
- **Advanced Indexing**: Label-based and property-based indexes with automatic index management.
- **Flexible Schema**: Dynamic property system supporting multiple data types (Int, Long, Double, String, Boolean, Blob).
- **State Management**: Temporal state tracking with state chains and blob chain management.
- **Query Engine**: Unified query interface with optimized query planning and execution strategies.

### Storage & Performance
- **Custom File Format**: Segment-based storage with checksums and compression support.
- **Memory Efficiency**: Smart caching with LRU eviction and dirty entity tracking.
- **Space Management**: Automatic space reclamation and segment management.
- **Deletion Handling**: Sophisticated tombstone management and reference updating.

### Developer Experience
- **Interactive REPL**: Full-featured command-line interface for database operations.
- **Traversal API**: Relationship-based graph traversal with BFS/DFS support.
- **Type Safety**: Strong type system with compile-time guarantees.
- **Comprehensive Testing**: Extensive test coverage with Google Test framework.

## Prerequisites

Before building, ensure you have the following installed:

- **C++ Compiler**: Clang 14+ or GCC 11+ (Must support C++20)
- **Build System**: [Meson](https://mesonbuild.com/) 0.60+ and [Ninja](https://ninja-build.org/)
- **Package Manager**: [Conan](https://conan.io/) 2.x
- **Python**: 3.10+
- **LLVM Tools**: `llvm-cov` and `llvm-profdata` (Required for coverage reports)

## Build & Test

We provide a convenient shell script to automate the environment setup, building, and testing process.

### One-Click Build (Recommended)

The `run_tests.sh` script handles dependency installation (Conan), configuration (Meson), compilation, unit testing, and coverage report generation.

```bash
# Clean build, install dependencies, compile, and run tests
./run_tests.sh

# Quick run (skip dependency installation if already installed)
./run_tests.sh --quick
```

After the script finishes, you can find the build artifacts in `buildDir/` and coverage reports in `buildDir/coverage/`.

### Manual Build Steps

If you prefer to run the steps manually or are integrating into a custom workflow:

1.  **Install Dependencies (Conan)**
    ```bash
    # Ensure C++20 and Debug mode are used
    conan profile detect
    conan install . --output-folder=buildDir --build=missing -s build_type=Debug -s compiler.cppstd=20
    ```

2.  **Configure Build (Meson)**
    ```bash
    # Configure with source-based coverage flags
    export FLAGS="-fprofile-instr-generate -fcoverage-mapping -std=c++20"
    
    meson setup buildDir \
      --native-file buildDir/conan_meson_native.ini \
      -Dcpp_args="$FLAGS" \
      -Dcpp_link_args="$FLAGS" \
      -Dbuildtype=debug
    ```

3.  **Compile**
    ```bash
    meson compile -C buildDir
    ```

4.  **Run Tests**
    ```bash
    # Set profile file location to avoid root directory pollution
    export LLVM_PROFILE_FILE="$(pwd)/buildDir/coverage/raw/code-%p.profraw"
    
    meson test -C buildDir
    ```

5.  **Generate Coverage Report**
    ```bash
    # macOS example (using xcrun)
    python3 scripts/generate_coverage.py buildDir buildDir/coverage/coverage.lcov "xcrun llvm-cov" "xcrun llvm-profdata"
    ```

## Usage

After building, the CLI executable is located at `buildDir/apps/cli/metrix-cli`.

### Database Management

```bash
# Create and open a new database
./buildDir/apps/cli/metrix-cli database create mydb.graph

# Open an existing database
./buildDir/apps/cli/metrix-cli database open mydb.graph
```

Once inside the REPL, type `help` to see all available commands for node/edge manipulation, querying, and indexing.

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built with [Boost](https://www.boost.org/) for cross-platform filesystem and system utilities.
- CLI powered by [CLI11](https://github.com/CLIUtils/CLI11).
- Testing with [Google Test](https://github.com/google/googletest).
- Build system: [Meson](https://mesonbuild.com/).
- Package management: [Conan](https://conan.io/).

## Contact

- **Repository**: [https://github.com/nexepic/metrix](https://github.com/nexepic/metrix)
- **Issues**: [GitHub Issues](https://github.com/nexepic/metrix/issues)
