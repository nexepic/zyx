# Algorithms

Metrix implements efficient algorithms optimized for graph database workloads.

## Overview

- [Overview](overview) - Algorithm categories and complexity analysis

## Storage Algorithms

- [Segment Allocation](segment-allocation) - O(1) allocation strategy
- [Bitmap Indexing](bitmap-indexing) - Space tracking with SIMD
- [Compression](compression) - Data compression techniques

## Transaction Algorithms

- [State Chain & Optimistic Locking](state-chain-optimistic-locking) - MVCC implementation
- [WAL Recovery](wal-recovery) - Crash recovery process

## Query Algorithms

- [Query Optimization](query-optimization) - Cost-based optimizer

## Caching Algorithms

- [Cache Eviction](cache-eviction) - LRU cache policy
