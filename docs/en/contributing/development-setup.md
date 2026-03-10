# Development Setup

## Prerequisites

- C++20 compiler
- Meson, Conan, Ninja
- Google Test
- ANTLR4 (for parser changes)

## Building

```bash
conan install . --output-folder=buildDir --build=missing
meson setup buildDir
meson compile -C buildDir
```

## Testing

```bash
./scripts/run_tests.sh
```
