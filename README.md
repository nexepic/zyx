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
  High-performance embeddable graph database engine in C++20 with Cypher support.
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

## What ZYX Provides

- Cypher query engine (`MATCH` / `CREATE` / `MERGE` / `WITH` / `UNION` / `UNWIND` / `CALL` / `LOAD CSV`)
- ACID transactions with WAL and recovery
- Schema/index administration (`CREATE INDEX`, constraints, vector index)
- Built-in procedures for DB stats/config, vector search/training, and GDS algorithms
- CLI modes for interactive REPL, script execution, and bulk import

Feature support details: [`UNSUPPORTED_CYPHER_FEATURES.md`](UNSUPPORTED_CYPHER_FEATURES.md)

## Prerequisites

- C++ compiler with C++20 support (Clang 14+ or GCC 11+)
- Meson 0.60+
- Ninja
- Conan 2.x
- Python 3.10+

## Build and Test

```bash
# Full build + tests + coverage
./scripts/run_tests.sh

# Quick local run (skip Conan install stage)
./scripts/run_tests.sh --quick
```

Useful commands:

```bash
meson test -C buildDir <test_name>
./scripts/build_release.sh
```

## Quick Start

Most users can run ZYX directly from a pre-built executable or package-manager installation.
Building from source is optional.

### 1) Install or download the CLI executable

```bash
zyx --help
```

If the executable is not on `PATH`, use the full path (for example `./zyx` or `./buildDir/apps/cli/zyx`).

### 2) Create and open a database

```bash
zyx database create ./demo.zyx
```

Open existing DB:

```bash
zyx database open ./demo.zyx
```

### 3) Run first queries (in REPL)

```cypher
CREATE (a:User {name: 'Alice'});
CREATE (b:User {name: 'Bob'});
MATCH (a:User {name: 'Alice'}), (b:User {name: 'Bob'})
CREATE (a)-[:KNOWS {since: 2026}]->(b);
MATCH (a:User)-[r:KNOWS]->(b:User)
RETURN a.name, b.name, r.since;
```

## CLI Modes

```bash
# Script mode
zyx database exec ./demo.zyx ./seed.cypher

# Bulk import (CSV / JSONL)
zyx import \
  --database ./demo.zyx \
  --nodes ./nodes.csv \
  --relationships ./rels.csv
```

## Documentation

- User guide (EN): `docs/apps/docs/content/docs/en/zyx/user-guide/`
- User guide (ZH): `docs/apps/docs/content/docs/zh/zyx/user-guide/`
- Architecture docs: `docs/apps/docs/content/docs/{en,zh}/zyx/architecture/`
- Contribution guide: [`CONTRIBUTING.md`](CONTRIBUTING.md)

## License

Apache License 2.0. See [`LICENSE`](LICENSE).
