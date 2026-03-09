# VitePress Documentation System Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Set up a bilingual VitePress documentation system with English and Chinese support, deployable to GitHub Pages.

**Architecture:** VitePress static site generator with locale-based routing (`/en/` for English, `/zh/` for Chinese), manual language toggle via navigation, deployed via GitHub Actions on push to main branch.

**Tech Stack:** VitePress 1.x, Vue 3, TypeScript, pnpm, GitHub Actions, GitHub Pages

---

## Prerequisites

- Node.js 20+ installed
- pnpm 9+ installed
- Git repository with GitHub Pages enabled
- `.worktrees/` directory exists and is gitignored

---

### Task 1: Create Git Worktree for Documentation Development

**Files:**
- Worktree: `.worktrees/vitepress-docs/`
- Branch: `feature/vitepress-docs`

**Step 1: Create the worktree with new branch**

```bash
git worktree add .worktrees/vitepress-docs -b feature/vitepress-docs
```

Expected: Worktree created at `.worktrees/vitepress-docs/`

**Step 2: Navigate to worktree**

```bash
cd .worktrees/vitepress-docs
```

**Step 3: Verify you're on the correct branch**

```bash
git branch --show-current
```

Expected: `feature/vitepress-docs`

---

### Task 2: Initialize VitePress in docs Directory

**Files:**
- Create: `docs/package.json`
- Create: `docs/pnpm-lock.yaml` (auto-generated)

**Step 1: Navigate to docs directory**

```bash
cd docs
```

**Step 2: Initialize package.json**

```bash
cat > package.json << 'EOF'
{
  "name": "metrix-docs",
  "version": "1.0.0",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "vitepress dev",
    "build": "vitepress build",
    "preview": "vitepress preview"
  },
  "devDependencies": {
    "vitepress": "^1.6.0",
    "vue": "^3.5.13"
  }
}
EOF
```

**Step 3: Install dependencies**

```bash
pnpm install
```

Expected: `node_modules/` and `pnpm-lock.yaml` created

**Step 4: Create .gitignore for node_modules**

```bash
cat > .gitignore << 'EOF'
node_modules
.DS_Store
.vitepress/cache
EOF
```

**Step 5: Commit**

```bash
git add package.json pnpm-lock.yaml .gitignore
git commit -m "feat: initialize VitePress project structure"
```

---

### Task 3: Create VitePress Configuration Structure

**Files:**
- Create: `docs/.vitepress/config/index.ts`
- Create: `docs/.vitepress/config/en.ts`
- Create: `docs/.vitepress/config/zh.ts`

**Step 1: Create config directory**

```bash
mkdir -p .vitepress/config
```

**Step 2: Create main configuration file**

```bash
cat > .vitepress/config/index.ts << 'EOF'
import { defineConfig } from 'vitepress'
import en from './en'
import zh from './zh'

export default defineConfig({
  title: 'Metrix',
  description: 'High-performance graph database engine',

  locales: {
    root: {
      label: 'English',
      lang: 'en-US',
      themeConfig: en
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      themeConfig: zh
    }
  },

  markdown: {
    lineNumbers: true
  },

  themeConfig: {
    outline: {
      level: [2, 3]
    }
  }
})
EOF
```

**Step 3: Create English configuration**

```bash
cat > .vitepress/config/en.ts << 'EOF'
export default {
  nav: [
    { text: 'User Guide', link: '/en/user-guide/installation' },
    { text: 'API Reference', link: '/en/api/cpp-api' },
    { text: 'Architecture', link: '/en/architecture/overview' },
    { text: 'Contributing', link: '/en/contributing/development-setup' },
    {
      text: 'Language',
      items: [
        { text: 'English', link: '/en/' },
        { text: '简体中文', link: '/zh/' }
      ]
    }
  ],

  sidebar: {
    '/en/': [
      {
        text: 'User Guide',
        items: [
          { text: 'Installation', link: '/en/user-guide/installation' },
          { text: 'Quick Start', link: '/en/user-guide/quick-start' },
          { text: 'Basic Operations', link: '/en/user-guide/basic-operations' }
        ]
      },
      {
        text: 'API Reference',
        items: [
          { text: 'C++ API', link: '/en/api/cpp-api' },
          { text: 'C API', link: '/en/api/c-api' },
          { text: 'Types', link: '/en/api/types' }
        ]
      },
      {
        text: 'Architecture',
        items: [
          { text: 'Overview', link: '/en/architecture/overview' },
          { text: 'Storage System', link: '/en/architecture/storage' },
          { text: 'Query Engine', link: '/en/architecture/query-engine' },
          { text: 'Transactions', link: '/en/architecture/transactions' }
        ]
      },
      {
        text: 'Contributing',
        items: [
          { text: 'Development Setup', link: '/en/contributing/development-setup' },
          { text: 'Testing', link: '/en/contributing/testing' },
          { text: 'Code Style', link: '/en/contributing/code-style' }
        ]
      }
    ]
  },

  socialLinks: [
    { icon: 'github', link: 'https://github.com/yuhong/metrix' }
  ],

  footer: {
    message: 'Released under the MIT License.',
    copyright: 'Copyright © 2025-present Metrix Contributors'
  }
}
EOF
```

**Step 4: Create Chinese configuration**

```bash
cat > .vitepress/config/zh.ts << 'EOF'
export default {
  nav: [
    { text: '用户指南', link: '/zh/user-guide/installation' },
    { text: 'API 参考', link: '/zh/api/cpp-api' },
    { text: '架构设计', link: '/zh/architecture/overview' },
    { text: '贡献指南', link: '/zh/contributing/development-setup' },
    {
      text: '语言',
      items: [
        { text: 'English', link: '/en/' },
        { text: '简体中文', link: '/zh/' }
      ]
    }
  ],

  sidebar: {
    '/zh/': [
      {
        text: '用户指南',
        items: [
          { text: '安装', link: '/zh/user-guide/installation' },
          { text: '快速开始', link: '/zh/user-guide/quick-start' },
          { text: '基本操作', link: '/zh/user-guide/basic-operations' }
        ]
      },
      {
        text: 'API 参考',
        items: [
          { text: 'C++ API', link: '/zh/api/cpp-api' },
          { text: 'C API', link: '/zh/api/c-api' },
          { text: '类型定义', link: '/zh/api/types' }
        ]
      },
      {
        text: '架构设计',
        items: [
          { text: '概述', link: '/zh/architecture/overview' },
          { text: '存储系统', link: '/zh/architecture/storage' },
          { text: '查询引擎', link: '/zh/architecture/query-engine' },
          { text: '事务管理', link: '/zh/architecture/transactions' }
        ]
      },
      {
        text: '贡献指南',
        items: [
          { text: '开发环境', link: '/zh/contributing/development-setup' },
          { text: '测试指南', link: '/zh/contributing/testing' },
          { text: '代码规范', link: '/zh/contributing/code-style' }
        ]
      }
    ]
  },

  socialLinks: [
    { icon: 'github', link: 'https://github.com/yuhong/metrix' }
  ],

  footer: {
    message: '基于 MIT 许可证发布',
    copyright: 'Copyright © 2025-present Metrix Contributors'
  }
}
EOF
```

**Step 5: Commit**

```bash
git add .vitepress/
git commit -m "feat: add VitePress configuration with i18n support"
```

---

### Task 4: Create English Homepage

**Files:**
- Create: `docs/en/index.md`

**Step 1: Create en directory**

```bash
mkdir -p en
```

**Step 2: Create English homepage**

```bash
cat > en/index.md << 'EOF'
---
layout: home

hero:
  name: Metrix
  text: High-Performance Graph Database
  tagline: Embeddable C++20 graph database engine with Cypher query support
  actions:
    - theme: brand
      text: Get Started
      link: /en/user-guide/installation
    - theme: alt
      text: View on GitHub
      link: https://github.com/yuhong/metrix

features:
  - title: High Performance
    details: Custom file-based storage with segment-based architecture for optimal speed.
  - title: ACID Transactions
    details: Full transaction support with Write-Ahead Logging (WAL) for data durability.
  - title: Cypher Query
    details: Native support for the Cypher query language with ANTLR4-based parser.
  - title: Vector Indexes
    details: Built-in support for vector similarity search and AI workloads.
  - title: Embeddable
    details: Easy integration into C++ applications with both C++ and C APIs.
  - title: Open Source
    details: MIT licensed for easy inclusion in your projects.
EOF
```

**Step 3: Commit**

```bash
git add en/
git commit -m "feat: add English homepage"
```

---

### Task 5: Create Chinese Homepage

**Files:**
- Create: `docs/zh/index.md`

**Step 1: Create zh directory**

```bash
mkdir -p zh
```

**Step 2: Create Chinese homepage**

```bash
cat > zh/index.md << 'EOF'
---
layout: home

hero:
  name: Metrix
  text: 高性能图数据库
  tagline: 可嵌入的 C++20 图数据库引擎，支持 Cypher 查询语言
  actions:
    - theme: brand
      text: 快速开始
      link: /zh/user-guide/installation
    - theme: alt
      text: 查看 GitHub
      link: https://github.com/yuhong/metrix

features:
  - title: 高性能
    details: 基于段的自定义文件存储架构，实现最优速度。
  - title: ACID 事务
    details: 完整的事务支持，通过预写日志 (WAL) 保证数据持久性。
  - title: Cypher 查询
    details: 基于 ANTLR4 的解析器，原生支持 Cypher 查询语言。
  - title: 向量索引
    details: 内置向量相似度搜索支持，适用于 AI 工作负载。
  - title: 可嵌入
    details: 提供丰富的 C++ 和 C API，轻松集成到应用程序中。
  - title: 开源
    details: MIT 许可证，便于在项目中使用。
EOF
```

**Step 3: Commit**

```bash
git add zh/
git commit -m "feat: add Chinese homepage"
```

---

### Task 6: Create Placeholder Content Pages (English)

**Files:**
- Create: `docs/en/user-guide/installation.md`
- Create: `docs/en/user-guide/quick-start.md`
- Create: `docs/en/user-guide/basic-operations.md`
- Create: `docs/en/api/cpp-api.md`
- Create: `docs/en/api/c-api.md`
- Create: `docs/en/api/types.md`
- Create: `docs/en/architecture/overview.md`
- Create: `docs/en/architecture/storage.md`
- Create: `docs/en/architecture/query-engine.md`
- Create: `docs/en/architecture/transactions.md`
- Create: `docs/en/contributing/development-setup.md`
- Create: `docs/en/contributing/testing.md`
- Create: `docs/en/contributing/code-style.md`

**Step 1: Create user-guide directory and pages**

```bash
mkdir -p en/user-guide

cat > en/user-guide/installation.md << 'EOF'
# Installation

## Prerequisites

- C++20 compatible compiler (GCC 11+, Clang 13+, MSVC 2022+)
- Meson build system
- Conan package manager
- Ninja build backend

## Build from Source

```bash
# Clone the repository
git clone https://github.com/yuhong/metrix.git
cd metrix

# Install dependencies and build
./scripts/run_tests.sh
```

## Quick Install

For detailed build options, see the [Contributing Guide](/en/contributing/development-setup).
EOF

cat > en/user-guide/quick-start.md << 'EOF'
# Quick Start

## Opening a Database

```cpp
#include <metrix/metrix.hpp>

using namespace metrix;

// Open or create a database
auto db = Database::open("./mydb");
```

## Creating a Transaction

```cpp
// Begin a transaction
auto tx = db->beginTransaction();
```

## Creating Nodes

```cpp
// Create a node with a label
auto node = tx->createNode("User");
node->setProperty("name", "Alice");
node->setProperty("age", 30);
```

## Querying with Cypher

```cpp
// Execute a Cypher query
auto result = db->executeQuery(
    "MATCH (u:User) WHERE u.name = 'Alice' RETURN u"
);
```

## Next Steps

- [Basic Operations](/en/user-guide/basic-operations)
- [API Reference](/en/api/cpp-api)
- [Architecture](/en/architecture/overview)
EOF

cat > en/user-guide/basic-operations.md << 'EOF'
# Basic Operations

## Creating Nodes

```cpp
auto person = tx->createNode("Person");
person->setProperty("name", "Bob");
```

## Creating Relationships

```cpp
auto alice = tx->createNode("Person");
auto bob = tx->createNode("Person");
auto knows = alice->createRelationshipTo(bob, "KNOWS");
knows->setProperty("since", 2020);
```

## Querying

See the [API Reference](/en/api/cpp-api) for detailed query operations.
EOF
```

**Step 2: Create api directory and pages**

```bash
mkdir -p en/api

cat > en/api/cpp-api.md << 'EOF'
# C++ API Reference

## Database Class

### `open()`

```cpp
static std::unique_ptr<Database> Database::open(const std::string& path)
```

Opens or creates a database at the specified path.

**Parameters:**
- `path`: File system path to the database directory

**Returns:** Unique pointer to Database instance

### `beginTransaction()`

```cpp
std::unique_ptr<Transaction> Database::beginTransaction()
```

Begins a new transaction.

**Returns:** Unique pointer to Transaction instance
EOF

cat > en/api/c-api.md << 'EOF'
# C API Reference

## `metrix_database_open()`

```c
METRIX_EXPORT metrix_database_t* metrix_database_open(const char* path);
```

Opens or creates a database.

## `metrix_database_close()`

```c
METRIX_EXPORT void metrix_database_close(metrix_database_t* db);
```

Closes and releases a database instance.
EOF

cat > en/api/types.md << 'EOF'
# Type Definitions

## Value Types

Metrix supports the following data types:

- **Null**: `Value()`
- **Boolean**: `Value(bool)`
- **Integer**: `Value(int64_t)`
- **Float**: `Value(double)`
- **String**: `Value(std::string)`
- **List**: `Value(std::vector<Value>)`
- **Map**: `Value(std::unordered_map<std::string, Value>)`
EOF
```

**Step 3: Create architecture directory and pages**

```bash
mkdir -p en/architecture

cat > en/architecture/overview.md << 'EOF'
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
EOF

cat > en/architecture/storage.md << 'EOF'
# Storage System

## Segment-Based Architecture

Metrix uses a custom file format with fixed-size segments for efficient space management.

## Components

- **FileHeaderManager**: Manages file-level metadata
- **SegmentTracker**: Tracks segment allocation
- **DataManager**: Handles node/edge/property data
- **IndexManager**: Manages label and property indexes
- **DeletionManager**: Tombstone management
- **CacheManager**: LRU cache with dirty tracking

## WAL Integration

All modifications are logged to the Write-Ahead Log before actual disk writes.
EOF

cat > en/architecture/query-engine.md << 'EOF'
# Query Engine

## Parser

ANTLR4-based Cypher parser with full language support.

## Planner

Converts parsed Cypher to logical plans with rule-based optimization.

## Executor

Executes physical plans with various operators:
- Scan operators: NodeScan, EdgeScan
- Modification: CreateNode, MergeNode, Delete
- Query: Filter, Sort, Project
- Special: VectorSearch, TrainVectorIndex
EOF

cat > en/architecture/transactions.md << 'EOF'
# Transaction Management

## ACID Properties

- **Atomicity**: All operations in a transaction succeed or fail together
- **Consistency**: Database always remains in valid state
- **Isolation**: Concurrent transactions don't interfere
- **Durability**: Committed changes survive failures

## Implementation

Optimistic locking with versioning and full rollback capabilities.
EOF
```

**Step 4: Create contributing directory and pages**

```bash
mkdir -p en/contributing

cat > en/contributing/development-setup.md << 'EOF'
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
EOF

cat > en/contributing/testing.md << 'EOF'
# Testing Guidelines

## Unit Tests

Located in `tests/src/` matching the source structure.

## Integration Tests

Located in `tests/integration/` for end-to-end workflows.

## Running Tests

```bash
# All tests
./scripts/run_tests.sh

# Specific test
meson test -C buildDir <test_name>

# With coverage
./scripts/run_tests.sh --html
```
EOF

cat > en/contributing/code-style.md << 'EOF'
# Code Style Guidelines

## C++ Standards

- C++20 features allowed
- Follow existing code patterns
- Use `auto` sparingly
- Const correctness required

## Commit Messages

- Use imperative mood: "Fix bug" not "Fixed bug"
- Be specific about what changed
- One logical change per commit

See [CONTRIBUTING.md](https://github.com/yuhong/metrix/blob/main/CONTRIBUTING.md) for full guidelines.
EOF
```

**Step 5: Commit**

```bash
git add en/
git commit -m "feat: add English documentation content"
```

---

### Task 7: Create Placeholder Content Pages (Chinese)

**Files:**
- Create: `docs/zh/user-guide/installation.md`
- Create: `docs/zh/user-guide/quick-start.md`
- Create: `docs/zh/user-guide/basic-operations.md`
- Create: `docs/zh/api/cpp-api.md`
- Create: `docs/zh/api/c-api.md`
- Create: `docs/zh/api/types.md`
- Create: `docs/zh/architecture/overview.md`
- Create: `docs/zh/architecture/storage.md`
- Create: `docs/zh/architecture/query-engine.md`
- Create: `docs/zh/architecture/transactions.md`
- Create: `docs/zh/contributing/development-setup.md`
- Create: `docs/zh/contributing/testing.md`
- Create: `docs/zh/contributing/code-style.md`

**Step 1: Create user-guide directory and pages**

```bash
mkdir -p zh/user-guide

cat > zh/user-guide/installation.md << 'EOF'
# 安装

## 前置要求

- C++20 兼容编译器 (GCC 11+, Clang 13+, MSVC 2022+)
- Meson 构建系统
- Conan 包管理器
- Ninja 构建后端

## 从源码构建

```bash
# 克隆仓库
git clone https://github.com/yuhong/metrix.git
cd metrix

# 安装依赖并构建
./scripts/run_tests.sh
```

## 快速安装

详细的构建选项，请参阅[贡献指南](/zh/contributing/development-setup)。
EOF

cat > zh/user-guide/quick-start.md << 'EOF'
# 快速开始

## 打开数据库

```cpp
#include <metrix/metrix.hpp>

using namespace metrix;

// 打开或创建数据库
auto db = Database::open("./mydb");
```

## 创建事务

```cpp
// 开始事务
auto tx = db->beginTransaction();
```

## 创建节点

```cpp
// 创建带标签的节点
auto node = tx->createNode("User");
node->setProperty("name", "Alice");
node->setProperty("age", 30);
```

## 使用 Cypher 查询

```cpp
// 执行 Cypher 查询
auto result = db->executeQuery(
    "MATCH (u:User) WHERE u.name = 'Alice' RETURN u"
);
```

## 下一步

- [基本操作](/zh/user-guide/basic-operations)
- [API 参考](/zh/api/cpp-api)
- [架构](/zh/architecture/overview)
EOF

cat > zh/user-guide/basic-operations.md << 'EOF'
# 基本操作

## 创建节点

```cpp
auto person = tx->createNode("Person");
person->setProperty("name", "Bob");
```

## 创建关系

```cpp
auto alice = tx->createNode("Person");
auto bob = tx->createNode("Person");
auto knows = alice->createRelationshipTo(bob, "KNOWS");
knows->setProperty("since", 2020);
```

## 查询

详细的查询操作请参阅 [API 参考](/zh/api/cpp-api)。
EOF
```

**Step 2: Create remaining Chinese pages**

```bash
mkdir -p zh/api zh/architecture zh/contributing

cat > zh/api/cpp-api.md << 'EOF'
# C++ API 参考

## Database 类

### `open()`

```cpp
static std::unique_ptr<Database> Database::open(const std::string& path)
```

打开或创建指定路径的数据库。

**参数:**
- `path`: 数据库目录的文件系统路径

**返回:** Database 实例的唯一指针
EOF

cat > zh/api/c-api.md << 'EOF'
# C API 参考

## `metrix_database_open()`

```c
METRIX_EXPORT metrix_database_t* metrix_database_open(const char* path);
```

打开或创建数据库。

## `metrix_database_close()`

```c
METRIX_EXPORT void metrix_database_close(metrix_database_t* db);
```

关闭并释放数据库实例。
EOF

cat > zh/api/types.md << 'EOF'
# 类型定义

## 值类型

Metrix 支持以下数据类型：

- **空值**: `Value()`
- **布尔值**: `Value(bool)`
- **整数**: `Value(int64_t)`
- **浮点数**: `Value(double)`
- **字符串**: `Value(std::string)`
- **列表**: `Value(std::vector<Value>)`
- **映射**: `Value(std::unordered_map<std::string, Value>)`
EOF

cat > zh/architecture/overview.md << 'EOF'
# 架构概述

Metrix 设计为分层系统，具有清晰的关注点分离。

## 分层架构

```
应用程序 (CLI, Benchmark)
       ↓
公共 API (C++ & C)
       ↓
查询引擎 (解析器 → 规划器 → 执行器)
       ↓
存储层 (FileStorage, WAL, 状态管理)
       ↓
核心 (数据库, 事务, 实体管理)
```
EOF

cat > zh/architecture/storage.md << 'EOF'
# 存储系统

## 段架构

Metrix 使用具有固定大小段的自定义文件格式，实现高效的空间管理。

## 组件

- **FileHeaderManager**: 管理文件级元数据
- **SegmentTracker**: 跟踪段分配
- **DataManager**: 处理节点/边/属性数据
- **IndexManager**: 管理标签和属性索引
- **DeletionManager**: 墓碑管理
- **CacheManager**: 带脏跟踪的 LRU 缓存
EOF

cat > zh/architecture/query-engine.md << 'EOF'
# 查询引擎

## 解析器

基于 ANTLR4 的 Cypher 解析器，提供完整的语言支持。

## 规划器

将解析的 Cypher 转换为逻辑计划，并进行基于规则的优化。

## 执行器

使用各种运算符执行物理计划：
- 扫描运算符: NodeScan, EdgeScan
- 修改运算符: CreateNode, MergeNode, Delete
- 查询运算符: Filter, Sort, Project
- 特殊运算符: VectorSearch, TrainVectorIndex
EOF

cat > zh/architecture/transactions.md << 'EOF'
# 事务管理

## ACID 属性

- **原子性**: 事务中的所有操作一起成功或一起失败
- **一致性**: 数据库始终保持有效状态
- **隔离性**: 并发事务互不干扰
- **持久性**: 已提交的更改在故障后仍然存在

## 实现

使用版本控制和完全回滚功能的乐观锁。
EOF

cat > zh/contributing/development-setup.md << 'EOF'
# 开发环境设置

## 前置要求

- C++20 编译器
- Meson, Conan, Ninja
- Google Test
- ANTLR4（用于解析器更改）

## 构建

```bash
conan install . --output-folder=buildDir --build=missing
meson setup buildDir
meson compile -C buildDir
```

## 测试

```bash
./scripts/run_tests.sh
```
EOF

cat > zh/contributing/testing.md << 'EOF'
# 测试指南

## 单元测试

位于 `tests/src/`，与源代码结构匹配。

## 集成测试

位于 `tests/integration/`，用于端到端工作流。

## 运行测试

```bash
# 所有测试
./scripts/run_tests.sh

# 特定测试
meson test -C buildDir <test_name>

# 生成覆盖率报告
./scripts/run_tests.sh --html
```
EOF

cat > zh/contributing/code-style.md << 'EOF'
# 代码规范指南

## C++ 标准

- 允许使用 C++20 特性
- 遵循现有代码模式
- 谨慎使用 `auto`
- 必须保持 const 正确性

## 提交消息

- 使用祈使语气："Fix bug" 而非 "Fixed bug"
- 具体说明更改内容
- 每次提交一个逻辑更改

完整指南请参阅 [CONTRIBUTING.md](https://github.com/yuhong/metrix/blob/main/CONTRIBUTING.md)。
EOF
```

**Step 3: Commit**

```bash
git add zh/
git commit -m "feat: add Chinese documentation content"
```

---

### Task 8: Create GitHub Actions Workflow

**Files:**
- Create: `.github/workflows/docs.yml`

**Step 1: Create workflows directory (if not exists)**

```bash
mkdir -p ../.github/workflows
```

**Step 2: Create the deployment workflow**

```bash
cat > ../.github/workflows/docs.yml << 'EOF'
name: Deploy Documentation

on:
  push:
    branches:
      - main
    paths:
      - 'docs/**'
      - '.github/workflows/docs.yml'
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: "pages"
  cancel-in-progress: false

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Setup pnpm
        uses: pnpm/action-setup@v4
        with:
          version: 9

      - name: Setup Node.js
        uses: actions/setup-node@v4
        with:
          node-version: '20'
          cache: 'pnpm'
          cache-dependency-path: 'docs/pnpm-lock.yaml'

      - name: Install dependencies
        working-directory: ./docs
        run: pnpm install

      - name: Build VitePress site
        working-directory: ./docs
        run: pnpm run build

      - name: Upload artifact
        uses: actions/upload-pages-artifact@v3
        with:
          path: ./docs/.vitepress/dist

  deploy:
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    runs-on: ubuntu-latest
    needs: build
    steps:
      - name: Deploy to GitHub Pages
        id: deployment
        uses: actions/deploy-pages@v4
EOF
```

**Step 3: Commit**

```bash
git add ../.github/workflows/docs.yml
git commit -m "feat: add GitHub Pages deployment workflow"
```

---

### Task 9: Update Base URL for GitHub Pages

**Files:**
- Modify: `docs/.vitepress/config/index.ts`

**Step 1: Update config with base URL**

```bash
cat > .vitepress/config/index.ts << 'EOF'
import { defineConfig } from 'vitepress'
import en from './en'
import zh from './zh'

export default defineConfig({
  // Base URL for GitHub Pages (update 'metrix' to your repo name)
  base: '/metrix/',

  title: 'Metrix',
  description: 'High-performance graph database engine',

  locales: {
    root: {
      label: 'English',
      lang: 'en-US',
      themeConfig: en
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      themeConfig: zh
    }
  },

  markdown: {
    lineNumbers: true
  },

  themeConfig: {
    outline: {
      level: [2, 3]
    }
  }
})
EOF
```

**Step 2: Commit**

```bash
git add .vitepress/config/index.ts
git commit -m "feat: configure base URL for GitHub Pages"
```

---

### Task 10: Local Testing

**Files:**
- Test: Local dev server

**Step 1: Start dev server**

```bash
cd docs
pnpm run dev
```

Expected output: Server running at `http://localhost:5173`

**Step 2: Test English site**

Open browser: `http://localhost:5173/en/`

Verify:
- [ ] Homepage loads
- [ ] Navigation links work
- [ ] Sidebar displays correctly
- [ ] Language toggle shows both options

**Step 3: Test Chinese site**

Open browser: `http://localhost:5173/zh/`

Verify:
- [ ] Homepage loads in Chinese
- [ ] Navigation is in Chinese
- [ ] Sidebar is in Chinese
- [ ] Language toggle works

**Step 4: Test language switching**

Click language toggle in navigation

Verify:
- [ ] Switching from /en/ to /zh/ works
- [ ] Switching from /zh/ to /en/ works
- [ ] URL updates correctly

**Step 5: Build verification**

```bash
pnpm run build
```

Expected: Build completes successfully with no errors

**Step 6: Preview build**

```bash
pnpm run preview
```

Open browser: `http://localhost:4173/metrix/en/`

Verify:
- [ ] Built site works correctly
- [ ] All pages accessible
- [ ] No console errors

**Step 7: Stop dev servers**

Press `Ctrl+C` in terminal(s)

---

### Task 11: Create README for Documentation

**Files:**
- Create: `docs/README.md`

**Step 1: Create README**

```bash
cat > README.md << 'EOF'
# Metrix Documentation

This directory contains the VitePress documentation for the Metrix graph database project.

## Local Development

### Prerequisites

- Node.js 20+
- pnpm 9+

### Install Dependencies

```bash
pnpm install
```

### Development Server

```bash
pnpm run dev
```

The site will be available at `http://localhost:5173`

### Build

```bash
pnpm run build
```

The built site will be in `.vitepress/dist/`

### Preview Build

```bash
pnpm run preview
```

## Languages

- **English**: `/en/` (primary)
- **Chinese**: `/zh/` (secondary)

## Deployment

Documentation is automatically deployed to GitHub Pages when changes are pushed to the `main` branch.

## Adding Content

1. Create or edit markdown files in `en/` or `zh/`
2. Update sidebar configuration in `.vitepress/config/en.ts` or `.vitepress/config/zh.ts`
3. Test locally with `pnpm run dev`
4. Commit and push to trigger deployment
EOF
```

**Step 2: Commit**

```bash
git add README.md
git commit -m "docs: add documentation README"
```

---

### Task 12: Verify All Files Committed

**Step 1: Check git status**

```bash
git status
```

Expected: No uncommitted changes

**Step 2: View commit history**

```bash
git log --oneline -10
```

Expected: All 12 commits visible

---

### Task 13: Push Feature Branch

**Step 1: Push to remote**

```bash
git push -u origin feature/vitepress-docs
```

**Step 2: Verify push succeeded**

Expected: Branch pushed to remote

---

## Post-Implementation Checklist

### GitHub Repository Setup

- [ ] Enable GitHub Pages in repository settings
  - Go to Settings → Pages
  - Source: GitHub Actions
  - Save

- [ ] Verify workflow runs
  - Check Actions tab for workflow execution
  - Ensure build and deploy jobs complete

- [ ] Verify deployment
  - Visit `https://<username>.github.io/metrix/`
  - Test both `/en/` and `/zh/` routes

### Merge to Main

- [ ] Create pull request from `feature/vitepress-docs` to `main`
- [ ] Review and merge PR
- [ ] Verify deployment triggers on merge

### Final Verification

- [ ] Documentation accessible at GitHub Pages URL
- [ ] All navigation links work
- [ ] Language toggle functions correctly
- [ ] Site is responsive on mobile devices
- [ ] No console errors in browser

---

## Next Steps

After implementation:

1. **Enhance Content**: Add detailed documentation for each section
2. **Add Diagrams**: Include architecture diagrams using Mermaid or external images
3. **Search**: Enable Algolia DocSearch for site search
4. **Versioning**: Add versioned documentation for releases
5. **Analytics**: Add analytics tracking (optional)
