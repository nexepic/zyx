### 2026-10-21 R5.1／R6.1：overlay拓扑适配与完整原型成本

证据：`benchmarks/architecture-design/decisions/20261021-r5-overlay-and-whole-cost/`。已修改overlay会通过拓扑页适配再生成reference，而非直接调用base reference入口；覆盖候选对照现同步接入单源topology页，先完整认证reference批，再按同一pin解析类型名（无类型token0保留空类型）。结果vector／名称及转换重叠容量均通过原buffer-admission报告，未验证的空名称不进入缓存；不复制完整属性集合。其他接口和默认路径不变。

使用真实LogicalGraphTransactionOverlay，加一个仅内存暂存且拒绝commit的测试transaction，在物理只读候选快照上暂存节点属性变化、删除和新增重复邻居边；同一个执行器与key-only对照相同，实际候选页计数非零。此用例不授予只读文件持久写权限。另核对类型名／无类型、结果存活期额度及转换拒绝退款。初版不兼容DISTINCT／行上限夹具，以及遗漏overlay拓扑适配导致计数为0的失败记录保留。

90边热点原型完整分配：key-only1703936B，保留canonical树并增加covered树后1835008B，增加一个128KiB索引池；退休0B、分配峰值1835008B、文件5373952B。完整成本包含原canonical树，未假设生产替换后可抵消，也不是时延收益或生产准入。重建56模块二进制，仅跑受影响查询／生命周期／coordinator／backend组：默认164通过，候选206通过。上一轮55模块879／1205加1条件跳过及各独立验证不重复计作本轮全跑。无全量单测或性能基准。**总计13/24（54.2%），验收增长0，不提交、不推送。**
