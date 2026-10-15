# Property predicate/container ownership

节点扫描的属性、范围、参数及复合谓词，遍历过滤、创建/合并属性映射、UNWIND 与过程参数数组接入复制前计费。原始映射/数组采用复制规范化，拒绝时保持旧值；自持映射的移动通过已准入空结构交换避免深拷贝，私有存储释放早于退款。710 项相关测试、移动调整后 441 项定向测试及 105 个独立案例通过，包含抽样 96 个缓存分配故障点和 6 种完整计划堆请求探针，未见漏计。仍有其他编译/运行工作区及稳态合同待闭环。

Component evidence only:22/34 (64.7%). Linux/clang19/libstdc++14; no Windows/macOS runtime, RSS, new timing campaign or Kuzu improvement claimed.
