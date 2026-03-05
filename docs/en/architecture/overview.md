# Architecture Overview

Metrix is designed as a layered system with clear separation of concerns.

## Layered Architecture

```
Applications (CLI, Benchmark)
       ↓
Public API (C++ & C)
       ↓
Query Engine (Parser → Planner → Executor)
       ↓
Storage Layer (FileStorage, WAL, State Management)
       ↓
Core (Database, Transaction, Entity Management)
```

## Key Design Principles

- **Embeddable**: Easy integration into host applications
- **ACID Compliant**: Full transaction support with rollback
- **Extensible**: Plugin-based architecture for custom indexes
