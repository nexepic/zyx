# 架构设计

Metrix 架构专为高性能、可扩展性和 ACID 合规性而设计。

## 核心架构

- [概述](overview) - 系统架构概述
- [存储系统](storage) - 基于段的存储引擎
- [查询引擎](query-engine) - 查询执行流水线
- [事务系统](transactions) - ACID 事务实现

## 存储细节

- [段格式](segment-format) - 磁盘存储格式
- [WAL 实现](wal) - 预写日志
- [缓存管理](cache) - 缓存策略

## 性能

- [优化策略](optimization) - 性能调优
- [基准测试](benchmarks) - 性能指标
