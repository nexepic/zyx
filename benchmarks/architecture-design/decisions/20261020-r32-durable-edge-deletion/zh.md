### 2026-10-20 R3.2：Edge、原生字段与双向邻接同次删除

证据：`benchmarks/architecture-design/decisions/20261020-r32-durable-edge-deletion/`。

- 同一显式owner暂存/发布入口加入Edge：先读取主Edge和两个端点Entity，认证由主记录构造的OUT/IN键均存在，再原子协调固定Edge页、Edge Map、字段/组live、保留目录、普通全局邻接和退休队列。保留主Edge作为拓扑权威，不引入覆盖读取准入。
- 共享固定map支持零字节suffix的key-only集合，直接复用既有17字节全局邻接键；两种格式均验证普通邻接读取。元数据私有脏页覆盖层仅额外准入这个确切namespace与17字节键宽，仍不触碰来源池、不整树重建、不放开其他namespace。
- Edge删除仍拒绝SystemCatalog/State根和LargeValue引用；尚未协调生产type索引/统计/约束。Node继续要求数据库逻辑无Edge且目标无标签；该检查用有界首项探测，允许“物理根仍在但逻辑为空”的Edge Map，同时拒绝孤儿邻接项。
- 最后一组Edge删除可清掉全局邻接根而保留端点，并恢复普通ROW Edge创建且ID不复用。自环的两个方向保持独立，parallel Edge及type 0主记录语义保留；旧pin不受新提交影响。
- 17个必要模块默认305通过；候选486通过、1个条件跳过。覆盖上述行为、删除后逆转换、暂存复制所有权、64 Edge/4096字段原1 MiB删除、A/B/取消及合法CRC、相同free-byte几何下缺失反向键的写前拒绝/恢复重试。没有全量单测或性能窗口。

初始损坏夹具给bulk page factory传了非零entry_count，已按已有合同清零再构建，失败记录保留。schema/标签/统计/type索引/大值生命周期、多组、Entity池碎片搬迁、生产采用和最终完整成本/同执行器ROW对照仍待完成。**没有新增阶段验收，总计13/24（54.2%）；不提交、不推送。**
