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
