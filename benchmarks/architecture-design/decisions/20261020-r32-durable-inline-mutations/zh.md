### 2026-10-20 R3.2：既有 inline extension 更新与删除

证据：`benchmarks/architecture-design/decisions/20261020-r32-durable-inline-mutations/`。

- 显式字段生命周期入口现在也认证既有 inline extension，支持标量/NULL替换与删除，并与同组列更新/降级合并最终计数。每批最多64个不同 owner/token，仍共享不超过1 MiB账户；缺失字段和 LargeValue 引用修改/删除继续在写盘前拒绝。
- 只复制实际相关字段页，删除旧 slot 后在已计费的私有页中放置替换，必要时 compact 或分配同批新逻辑页。extension目录精确旧suffix被替换，增长引发的定位变化不会保留重复前缀。group计数、owner/extension目录、物理页与整池退休仍由同一次 A/B 发布协调；删空字段保留实体live位和主Edge权威。
- 短scalar的相同值不发布，double按位比较以保存显式负零/NaN载荷。复杂inline来源只认证其借用编码，不为了替换而解码完整 owning值。旧slot记录的持有字节覆盖借用view生命周期。
- 15个相关模块：默认302通过；候选444通过、1个条件跳过。新增覆盖已有稀疏字段、列降级后的后续写入、混合列/extension同批、NULL到int增长、删空namespace、重复/缺失/no-op零写盘、Native事务、取消前后退款重试、旧pin、A/B恢复，以及 LargeValue引用保护。没有全量单测或性能基准。

尚未完成缺失字段插入/晋升、通用类型及大值引用回收、整owner/Entity Map联动、多组、搬迁和最终成本/同执行器ROW对照。**无新增阶段验收，总进度13/24（54.2%）；不提交、不推送。**
