# System Requirements

Metrix has specific requirements for building and running the database engine.

## Build Requirements

### Compilers

Metrix requires a C++20 compatible compiler:

- **GCC**: Version 11 or later
- **Clang**: Version 13 or later
- **MSVC**: Version 2022 or later (Visual Studio 17)
- **Apple Clang**: Version 14 or later (Xcode 14+)

### Build Tools

- **Meson**: Version 0.63 or later
- **Conan**: Version 1.60 or later
- **Ninja**: Version 1.10 or later
- **Python**: Version 3.8 or later (for build scripts)

### Dependencies

Metrix automatically manages its dependencies through Conan:

- **Boost**: Filesystem and system utilities
- **zlib**: Data compression
- **Google Test**: Testing framework
- **CLI11**: Command line interface
- **ANTLR4 C++ Runtime**: Parser generation (4.13.1)

## Runtime Requirements

### Operating Systems

Metrix is tested on:

- **Linux**: Ubuntu 20.04+, Debian 11+, CentOS 8+
- **macOS**: macOS 12+ (Monterey or later)
- **Windows**: Windows 10+ with MSVC 2022

### CPU Architecture

- **x86_64**: Primary supported architecture
- **ARM64**: Experimental support (Apple Silicon, ARM64 Linux)

### Memory Requirements

**Minimum**: 512 MB RAM
**Recommended**: 2 GB RAM or more

Memory usage scales with:
- Database size
- Cache configuration
- Concurrent transactions
- Query complexity

### Disk Space

**Minimum**: 100 MB free space
**Recommended**: 1 GB or more for production databases

Disk space is used for:
- Database files (segment-based storage)
- Write-Ahead Log (WAL)
- Indexes
- Temporary files during operations

## Platform-Specific Notes

### Linux

**Required System Packages**:
```bash
# Ubuntu/Debian
sudo apt-get install -y g++ python3-pip

# Fedora/RHEL
sudo dnf install -y gcc-c++ python3-pip
```

**Kernel Requirements**: Linux kernel 3.10 or later

### macOS

**Required Tools**: Xcode Command Line Tools
```bash
xcode-select --install
```

**Filesystem**: APFS or HFS+ recommended

### Windows

**Required Software**:
- Visual Studio 2022 with C++ development tools
- Python 3.8+ from python.org or Microsoft Store
- Git for Windows

**Build Environment**: Use Developer Command Prompt or PowerShell

## Performance Considerations

### Storage Performance

For optimal performance, use:
- **SSD**: Solid State Drive recommended for production
- **NVMe**: Best performance for high-throughput workloads
- **HDD**: Acceptable for development and low-throughput scenarios

### Network Filesystems

Metrix has not been tested on network filesystems (NFS, SMB, etc.). Use local filesystems for best results.

### CPU Requirements

Single-threaded performance is important for:
- Query execution
- Transaction processing
- Compression operations

Multi-core utilization for:
- Concurrent queries
- Parallel index operations
- Background compaction

## Development vs Production

### Development Requirements

For development and testing:
- 4 GB RAM
- 2 CPU cores
- 10 GB disk space
- SSD recommended but not required

### Production Requirements

For production deployments:
- 8 GB RAM or more
- 4+ CPU cores
- Local SSD storage
- Regular backup strategy

## Resource Limits

### Maximum Database Size

- **Theoretical**: 16 TB (limited by file system)
- **Tested**: Up to 1 TB

### Maximum Concurrent Connections

- **Default**: Unlimited (limited by system resources)
- **Recommended**: 100 concurrent transactions per CPU core

### Maximum Segment Size

- **Default**: 4 MB per segment
- **Configurable**: 1 MB to 1 GB

## Compatibility Matrix

| Compiler Version | Linux | macOS | Windows |
|-----------------|-------|-------|---------|
| GCC 11          | ✓     | -     | -       |
| GCC 12          | ✓     | -     | -       |
| GCC 13          | ✓     | -     | -       |
| Clang 13        | ✓     | ✓     | -       |
| Clang 14        | ✓     | ✓     | -       |
| Clang 15        | ✓     | ✓     | -       |
| Apple Clang 14  | -     | ✓     | -       |
| Apple Clang 15  | -     | ✓     | -       |
| MSVC 2022       | -     | -     | ✓       |

## Verification

After installation, verify your environment:

```bash
# Check compiler
g++ --version  # or clang++ --version

# Check build tools
meson --version
conan --version
ninja --version

# Check Python
python3 --version
```

## See Also

- [Installation](/en/contributing/installation) - Build and install instructions
- [Configuration](/en/contributing/configuration) - Runtime configuration options
- [Performance Optimization](/en/architecture/optimization) - Tuning guidelines
