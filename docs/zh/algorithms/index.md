# 算法详解

Metrix 实现了针对图数据库工作负载优化的高效算法。

## 算法概述

- [概述](overview) - 算法分类和复杂度分析

## 存储算法

- [段分配](segment-allocation) - O(1) 分配策略
- [位图索引](bitmap-indexing) - 使用 SIMD 的空间跟踪
- [数据压缩](compression) - 数据压缩技术

## 事务算法

- [状态链与乐观锁](state-chain-optimistic-locking) - MVCC 实现
- [WAL 恢复](wal-recovery) - 崩溃恢复过程

## 查询算法

- [查询优化](query-optimization) - 基于成本的优化器

## 缓存算法

- [缓存淘汰](cache-eviction) - LRU 缓存策略
