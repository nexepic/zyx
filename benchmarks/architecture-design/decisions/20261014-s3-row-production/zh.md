# S3.3 获准 ROW 放置接入生产

原生 RelocatableStorageBackend 已默认启用 S3.2 成本准入的 ROW chunk 策略。普通 NativeGraphWritePlanner 属性写入将策略传入 PropertyCollectionRootStore。新物化、符合条件的 RAW 集合使用有界切点；压缩、小集合、过宽、不准入的集合和通用流式/批量调用者保持既有合法 ROW extent 路径。调用者可显式关闭该放置优化。已有不可变引用及内容去重命中保留原布局读取，不做后台重打包或格式迁移。

双语原生单文件规范现已区分已实现的逐键变更 API 与仍存在的整 owner 重写放大，描述实际 ROW 策略及限制，并将旧的固定 16–64 KiB chunk 假设修正为现有显式偏移 Manifest 合同。所有受支持均匀 extent 类继续合法。切点仅为写者临时选项，reader 按原有 manifest/page owner/generation/checksum 协议认证两种布局，不需修改格式版本、页类型、根命名空间或 Driver ABI。owner-keyed Property Directory 继续是唯一权威，不启用 MIXED/列容器。

当前源码通过 **145 个相关测试**：属性/LargeValue/原生生命周期 130 个，S2.4 真实原生语义 7 个，绑定程序原生集成 8 个。新增公开 Database/QueryEngine 用例确认默认策略、选择性扫描/聚合、non-null COUNT、SET 结果、关闭后的大值拥有式输出及重开。默认启用的新鲜原生 smoke 通过 **13 类共 916 条完整结果命令**，包含写入、大值、旧快照、物理/逻辑只读、取消和低预算。证明及本地原始证据已保存。

验收 S3.3 获准有条件 ROW 放置及规范更新。完整 owner 替换仍可能重编码无关大字段字节，数据/Manifest/entry 成本估计不证明所有共享池/文件大小上限，进程级内存仍属 S5。专门故障矩阵属 S3.4，O3/O5 性能/资源及稳态验收保留在 S3.5/最终窗口。未运行全套或比较计时。
