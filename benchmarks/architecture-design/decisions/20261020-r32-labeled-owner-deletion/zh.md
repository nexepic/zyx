### 2026-10-20 R3.2：带标签 Node 的字段／实体／LabelSet 原子删除

证据：`benchmarks/architecture-design/decisions/20261020-r32-labeled-owner-deletion/`。受控Node删除现在认证完整inline及overflow标签，逐个删除其独占LabelSet槽；同一源LabelSet页的多次删除合并成一份私有图像。其他Node的稳定slot locator与标签内容保持不变；非空标签页采用同PageId的私有物理COW，空页移除当前Page Directory路由。LabelSet、固定Node／Entity Map、原生字段目录／组头、Catalog／allocator及退役队列仍在同一次A/B发布。

源页及free-byte提示经类型化认证；整个Entity池仅在所有当前帧都被替换／移除时退役。旧pin可继续读取被删Node及原标签，排空后受限整池释放生效。标签页及编码／复制缓冲沿用调用方账户；额度拒绝和私有准备后取消不写源页／根，退款后可独立重试。仍要求无schema／derived State协调需求，Node删除的拓扑边界仍保守保留，不能据此宣称通用图写入已完成。

原inline标签拒绝夹具已在实现接合后改为验证成功删除、保留另一owner及旧pin；保存了原失败记录。新增6项标签删除用例覆盖未转换拒绝、共享页邻居槽、合并空页移除、全部owner删除后的旧pin／池释放、额度／取消及A/B两种恢复。最终只运行31个相关存储模块（加入既有LabelSet存储／公开合同模块）：默认460通过；候选691通过、1个条件跳过。未执行全量单测或性能基准，仅验证Linux。

schema／统计／索引／State及通用拓扑事务接合、完整成本／同执行器ROW对照和R7仍待完成。**验收进度13/24（54.2%）；不提交、不推送。**
