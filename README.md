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

<h3 align="center">Embeddable Graph Database Engine</h3>

<p align="center">
  ACID-compliant graph database with Cypher queries, vector search, and graph algorithms — embeds anywhere from CLI to browser.
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
    <a href="https://nexepic.github.io/zyx">
        <img alt="Docs" src="https://img.shields.io/badge/Docs-nexepic.github.io/zyx-green.svg" />
    </a>
</p>

---

## Try It

```
$ zyx database create ./movies.zyx

<ZYX> Shell.
Type 'help' or 'exit'.
Enter queries ending with ';' OR press Enter on an empty line to execute.

zyx> CREATE (m:Movie {title: 'The Matrix', year: 1999}),
     ->        (a:Actor {name: 'Keanu Reeves'}),
     ->        (a)-[:ACTED_IN {role: 'Neo'}]->(m);
Empty result.

zyx> MATCH (a:Actor)-[r:ACTED_IN]->(m:Movie)
     -> RETURN a.name AS actor, m.title AS movie, r.role AS role;
+---------------+------------+------+
| actor         | movie      | role |
+---------------+------------+------+
| Keanu Reeves  | The Matrix | Neo  |
+---------------+------------+------+
(1 rows)

zyx> CALL db.stats();
+--------+-------+--------+
| nodes  | edges | labels |
+--------+-------+--------+
| 2      | 1     | 3      |
+--------+-------+--------+
(1 rows)
```

Or try it in the browser — [**Live Playground**](https://nexepic.github.io/zyx/en/playground) (no install required, runs via WebAssembly).

## Highlights

- **Cypher Query Engine** — `MATCH`, `CREATE`, `MERGE`, `WITH`, `UNION`, `UNWIND`, `CALL`, `LOAD CSV`
- **ACID Transactions** — Write-ahead logging, crash recovery, snapshot isolation
- **Vector Search** — HNSW index with cosine/euclidean/dot-product similarity
- **Graph Algorithms** — PageRank, shortest path, community detection via built-in procedures
- **Schema & Indexes** — Label indexes, property indexes, uniqueness constraints
- **Embeddable** — C++ header-only API, C API, Python bindings (`zyxdb`), and WebAssembly
- **Cross-Platform** — macOS, Linux, Windows

## Quick Start

### Install

Download a pre-built binary from [Releases](https://github.com/nexepic/zyx/releases), or build from source:

```bash
./scripts/run_tests.sh          # Full build + tests + coverage
./scripts/build_release.sh      # Release build only
```

Prerequisites: C++20 compiler (Clang 14+ / GCC 11+), Meson 0.60+, Ninja, Conan 2.x, Python 3.10+

### Use

```bash
zyx database create ./mydb.zyx      # Create new database
zyx database open ./mydb.zyx        # Open existing database
zyx database exec ./mydb.zyx q.cql  # Execute a Cypher script
zyx import --database ./mydb.zyx \
  --nodes nodes.csv \
  --relationships rels.csv          # Bulk CSV/JSONL import
```

### Embed

**C++**
```cpp
#include <zyx/zyx.hpp>

zyx::Database db("./mydb.zyx");
db.open();
auto result = db.query("MATCH (n) RETURN n.name LIMIT 5");
db.close();
```

**Python**
```python
import zyxdb

db = zyxdb.Database("./mydb.zyx")
db.open()
result = db.execute("MATCH (n) RETURN n.name LIMIT 5")
db.close()
```

## Documentation

Full docs with architecture deep-dives, API reference, and algorithm guides:

**[nexepic.github.io/zyx](https://nexepic.github.io/zyx)**

- [User Guide](https://nexepic.github.io/zyx/docs/zyx/user-guide/quick-start)
- [API Reference](https://nexepic.github.io/zyx/docs/zyx/api/cpp-api)
- [Architecture](https://nexepic.github.io/zyx/docs/zyx/architecture/overview)
- [Contributing](CONTRIBUTING.md)

Feature support details: [`UNSUPPORTED_CYPHER_FEATURES.md`](UNSUPPORTED_CYPHER_FEATURES.md)

## License

Apache License 2.0. See [`LICENSE`](LICENSE).
