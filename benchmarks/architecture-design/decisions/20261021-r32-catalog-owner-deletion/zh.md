### 2026-10-21 R3.2／R6.1：带schema的实体删除与统计原子协调

证据：`benchmarks/architecture-design/decisions/20261021-r32-catalog-owner-deletion/`。有界Node／Edge删除现移除实际label/type、property/composite及约束支持键，和Entity Map、字段目录、双向邻接及退休一起发布。只更新已知图计数和完整label/type计数；未知统计保留未知、未完整的旧计数记录不改写，计数下溢在物理写入前拒绝。SC私有BTree路径仅允许规范计数upsert，不授予token／schema／配置的任意修改权限；单独观察private_catalog_index_images。

移除“全库必须无边”的临时限制：通过目标Node的OUT／IN前缀各一次认证seek证明孤立；有incident Edge仍拒绝，无全局邻接根且存在Edge时也拒绝。没有扫描无关Edge或把数据库规模变成单次删除上限。标量／支持树保留原键编码和源一致性检查，8192总派生差分、4096受影响schema token及1MiB原账户保持。

新增8个删除／统计回归，以及同执行器公开后端对照，覆盖孤立Node、incident拒绝、Node／Edge所有派生状态、已知／未知统计、图及label计数下溢、A/B、旧pin、完整性扫描、64KiB额度退款、SC页准备后取消／重试。首次用例误要求COW改变逻辑根ID；现验证实际物理路由及新旧内容，失败日志保留。50相关模块：默认739通过；候选1061通过、1项条件跳过。成熟多组空间用例保留独立证据，无全库单测／性能基准。普通拓扑写入及VECTOR协调、剩余R5／R7继续待办。**总计13/24（54.2%），验收增长0，不提交、不推送。**
