<p align="center">
    <br />
    <a href="https://github.com/nexepic/zyx">
        <picture>
            <source srcset="./assets/branding/icon-light.svg" media="(prefers-color-scheme: dark)">
            <img src="./assets/branding/icon.svg" alt="ZYX Logo" width="120" />
        </picture>
    </a>
    <br />
    <br />
</p>

<p align="center">
  A high-performance, embeddable graph database engine, designed for efficient storage, indexing, and querying of graph-structured data.
</p>

<p align="center">
    <a href="https://github.com/nexepic/zyx/actions/workflows/build.yml">
        <img alt="Build Status" src="https://github.com/nexepic/zyx/actions/workflows/build.yml/badge.svg" />
    </a>
    <a href="https://codecov.io/github/nexepic/zyx">
        <img alt="codecov" src="https://codecov.io/github/nexepic/zyx/graph/badge.svg?token=VBCGBBI4YO" />
    </a>
    <a href="LICENSE">
        <img alt="License: Apache 2.0" src="https://img.shields.io/badge/License-Apache_2.0-blue.svg" />
    </a>
    <a href="https://en.cppreference.com/w/cpp/20">
        <img alt="C++" src="https://img.shields.io/badge/C++-20-blue.svg" />
    </a>
    <a href="https://mesonbuild.com/">
        <img alt="Meson" src="https://img.shields.io/badge/Build-Meson-blue.svg" />
    </a>
</p>

## Features

### Core Capabilities

- **Native Graph Storage**: Purpose-built data structures for nodes, edges, and properties with efficient serialization.
- **ACID Transactions**: Full transaction support with rollback capabilities and Write-Ahead Logging (WAL).
- **Advanced Indexing**: Label-based and property-based indexes with automatic index management.
- **Flexible Schema**: Dynamic property system supporting multiple data types (Int, Long, Double, String, Boolean,
  Blob).
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

The `run_tests.sh` script handles dependency installation (Conan), configuration (Meson), compilation, unit testing, and
coverage report generation.

```bash
# Clean build, install dependencies, compile, and run tests
./run_tests.sh

# Quick run (skip dependency installation if already installed)
./run_tests.sh --quick
```

After the script finishes, you can find the build artifacts in `buildDir/` and coverage reports in `buildDir/coverage/`.

### Manual Build Steps

If you prefer to run the steps manually or are integrating into a custom workflow:

1. **Install Dependencies (Conan)**
   ```bash
   # Ensure C++20 and Debug mode are used
   conan profile detect
   conan install . --output-folder=buildDir --build=missing -s build_type=Debug -s compiler.cppstd=20
   ```

2. **Configure Build (Meson)**
   ```bash
   # Configure with source-based coverage flags
   export FLAGS="-fprofile-instr-generate -fcoverage-mapping -std=c++20"
   
   meson setup buildDir \
     --native-file buildDir/conan_meson_native.ini \
     -Dcpp_args="$FLAGS" \
     -Dcpp_link_args="$FLAGS" \
     -Dbuildtype=debug
   ```

3. **Compile**
   ```bash
   meson compile -C buildDir
   ```

4. **Run Tests**
   ```bash
   # Set profile file location to avoid root directory pollution
   export LLVM_PROFILE_FILE="$(pwd)/buildDir/coverage/raw/code-%p.profraw"
   
   meson test -C buildDir
   ```

5. **Generate Coverage Report**
   ```bash
   # macOS example (using xcrun)
   python3 scripts/generate_coverage.py buildDir buildDir/coverage/coverage.lcov "xcrun llvm-cov" "xcrun llvm-profdata"
   ```

## Usage

After building, the CLI executable is located at `buildDir/apps/cli/zyx`.

### Database Management

```bash
# Create and open a new database
./buildDir/apps/cli/zyx database create mydb.graph

# Open an existing database
./buildDir/apps/cli/zyx database open mydb.graph
```

Once inside the REPL, type `help` to see all available commands for node/edge manipulation, querying, and indexing.

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

This project is licensed under the Apache License v2.0.<br /> - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built with [Boost](https://www.boost.org/) for cross-platform filesystem and system utilities.
- CLI powered by [CLI11](https://github.com/CLIUtils/CLI11).
- Testing with [Google Test](https://github.com/google/googletest).
- Build system: [Meson](https://mesonbuild.com/).
- Package management: [Conan](https://conan.io/).

## Contact

- **Repository**: [https://github.com/nexepic/zyx](https://github.com/nexepic/zyx)
- **Issues**: [GitHub Issues](https://github.com/nexepic/zyx/issues)
