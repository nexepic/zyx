### 2026-10-20 R3.2：新独立大值与内容索引原子发布

证据：`benchmarks/architecture-design/decisions/20261020-r32-fresh-large-publication/`。

显式 ROW→hybrid 现能发布新独立字段大值。私有数据/manifest 帧、manifest 目录、新内容索引项、字段引用、allocator/Catalog/Page Directory及源池精确退休同属一次 A/B；同批相同单字段载荷只分配一个 ID。已有内容索引保留 U64 叶布局，只复制变化路径；不重建整树，不原地修改源物理池。内容树子页按其 pin 下 Page Directory 核对物理代际，根仍保留精确 Index Root fence。字段及旧 ROW carrier 的跨 owner 退休判断没有放宽。

新增6项定向回归通过：Node/Edge、新身份和内容索引去重、旧 pin/只读重开、A/B完整恢复、原1 MiB额度内的128 KiB拒绝和私有准备后取消退款重试、多层内容树96个其他media值保持及未触及页不变、源内容索引池等pin后释放。首轮旧测试仍期待已撤销的“新内容一律拒绝”，已改为 preparation-only 不发布测试；新 fixture 的引用准入/命名空间/退役请求类型错误已修正，保留全部失败日志。

最终仅22个相关存储模块回归：默认366通过；候选566通过、1个条件跳过。未运行全量单测或性能基准。已同步隔离格式规范，默认生产格式31不采用该布局。通用大值 SET/引用删除、跨owner GC、复杂超大ROW逆转换、schema/统计/标签协调、多组、完整成本/同执行器ROW对照及生产准入仍未完成。**没有新增阶段验收，13/24（54.2%），不提交、不推送。**
