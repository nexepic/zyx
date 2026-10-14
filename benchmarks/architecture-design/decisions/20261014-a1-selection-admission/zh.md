# A1 选择向量真实分配准入

日期：2026-10-14。状态：S2 生产执行器的选择位图预算迁移完成并通过定向验证；完整 S2 尚未验收。总进度保持 5/34（14.7%），没有新增验收项。

SelectionVector 改用 PMR vector。查询运行时的五处位图分配使用共享 MaterializedByteBudget 对应的 BudgetMemoryResource，在 upstream 分配前按实际请求准入，释放实际容量时归还；移除独立的手工位图 reservation，避免重复计费。requiredStorageBytes 仍用于预取规划，storageBytes 返回实际容量。复制构造保留源 allocator，赋值保留目标 allocator；跨资源移动在目标预算中先分配成功再发布，源残留容量在析构前继续计费。失败保持原选择状态并归还未成功分配的预算。resource 必须活过所有位图 owner 和复制。

五处接入为属性预取、直接 Edge COUNT、直接 Edge 选列读取、NodeScan 可选谓词位图、邻接遍历选列读取。可选 NodeScan 只在过滤及用户可观察效果之前拒绝位图预算准入；bad_alloc、取消及损坏错误继续传播。独立 LogicalGraphReadExecutor 的位图尚未接入同一共享预算协议；其他容器、完整批次/arena、所有算子和进程级资源协调仍未完成，不宣称全执行器配额已完成。

定向扩展验证发现 GroupedMixedCountOmitsPayloadOnlyWithNonNullCapability 在原 4096 字节限额下失败。对照使用已提交 HEAD 的 LogicalPlanReadRuntime.cpp、当前 SelectionVector 头文件和相同模块对象，也在分组 scratch 准入处失败；因此不能把该对照称为完整旧版本构建。诊断观察为有效预算 3816、已用 3773，分组临时数组请求 64 字节，剩余仅 43。诊断代码仅保留于 /tmp，不进入生产。借用列在回调期间仍驻留，而原输入准入预留的两行空间在列 enrichment 后不再成立。

修复在借用列 enrichment 后、交付任何行之前重新检查 bitmap 加最大 enriched 行的两倍余量。多行批次不足时沿既有 PrefetchBytePressure 路径拆小，避免重放聚合/公开回调；单行真正超限仍失败。最大行大小合并到已有扫描中，不新增一次行遍历。原测试预算、COUNT/SUM 结果断言及列读取键断言均保持不变。

最终模块：runtime 222 项/5 suites、admission 68 项/5 suites，后者为前者子集，共 222 项独立测试通过。新增 8 项位图测试覆盖分配前拒绝、实际容量释放、upstream bad_alloc 退款/重试、复制资源与独立位、赋值失败原子性、同资源移动、跨资源移动与失败、零长度及空 resource 拒绝。现有测试覆盖列过滤、Node 聚合、Edge 批量读/聚合与拓扑预过滤。Linux/C++20 验证；未运行全量测试、性能基准或 Kuzu 比较，不宣称跨平台已实测。编译有既有 property_tokens 缺省初始化警告，不宣称 warnings-as-errors。

证据：同名 results 目录保存最终日志、模块链接命令、源文件 SHA-256 与 verification.json。工作基线为 5b8cd519；本轮没有验收比例提升；用户随后明确授权将本轮已验证的改动与证据提交并推送，作为部分 S2 检查点。S2.1 的完整物理绑定、批次身份及动态值 owner 和 S2.2–S2.5 的生产消费者/语义/最终对照继续推进。
