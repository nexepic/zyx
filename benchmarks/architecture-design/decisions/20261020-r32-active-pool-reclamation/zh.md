### 2026-10-20 R3.2：活跃候选的受控整池回收与可写重开

证据：`benchmarks/architecture-design/decisions/20261020-r32-active-pool-reclamation/`。

已验证通过现有延迟 reconciliation 模式重开可写元数据、继续 Native 事务列修改、再次只读重开验证，再逆转换回 ROW 恢复普通写入。没有绕过普通池的保护性拒绝。

`tryReleaseRetiredCatalogSegments` 对活跃候选增加受控分支：每批最多64个已退休 Property/Index 池；先核对持久队列、Catalog身份/epoch与快照屏障，再逐池最多31个 frame 认证并拒绝当前路由，随后一次 A/B 发布清队列、释放 allocator 位和删除这些确切 Catalog 身份。该分支不执行全 Catalog 历史扫描，不改字段 bytes/描述符，不放开逐帧 reconciliation 或通用搬迁。未退休或其他池种仍拒绝，旧 pin 未释放则整批不执行。

五轮持续 native 列修改直接回收/复用通过，不再要求每次先逆转换，且已释放池的 Catalog 身份不逐轮累积。覆盖旧 pin 阻挡、A/B新旧完整状态、过大批次、重复/陈旧身份和源池损坏拒绝后的恢复重试。仍未新增任务验收；**总进度13/24（54.2%），未提交/推送，未运行性能基准。** 通用字段增删/类型变化、整 owner 删除、多组目录、大值引用生命周期、搬迁及完整成本验收仍是后续工作。
