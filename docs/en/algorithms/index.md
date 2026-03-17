# Algorithms

ZYX implements efficient algorithms optimized for graph database workloads.

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

## Indexing Algorithms

- [B+Tree Indexing](btree-indexing) - B+Tree structure and operations
- [Label Index](label-index) - Node label indexing with B+Tree
- [Property Index](property-index) - Type-specific property indexing

## Vector Algorithms

- [DiskANN](diskann) - Disk-based Approximate Nearest Neighbor search
- [Product Quantization](product-quantization) - Vector compression for fast search
- [K-Means Clustering](kmeans) - Clustering for PQ codebook training
- [Vector Metrics](vector-metrics) - Distance computation and similarity

## Traversal Algorithms

- [Relationship Traversal](relationship-traversal) - Linked-list based graph traversal
