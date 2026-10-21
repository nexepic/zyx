### 2026-10-20 R3.2：既有列字段删除与类型变化的真实目录提交

证据：`benchmarks/architecture-design/decisions/20261020-r32-durable-field-demotion/`。

- 显式 `field_lifecycle` 路径支持同组最多64个既有列 cell 的标量修改/删除，仍使用上限1 MiB账户。`erase=true` 删除成员；`monostate` 且 `erase=false` 保存 NULL。默认兼容列入口的限制保留，Native事务仍复制暂存并禁止混用 Entity/ID/普通 Property/索引/Vector。
- 同列全部变更先合并再判定最终48槽阈值，允许同批兼容修改、成员删除和列降级。字段页、owner/column/extension 目录、Index Root Directory、描述符及整池退休一次提交；清空的字段页与空目录根一同移除。owner 存活及 Entity Map 不受字段删除影响。
- 目录更新复用现有 B+Tree 覆盖层，增加元数据私有镜像模式：仅生成脏路径页，不写来源池、不执行容量搜索/reconciliation/可选整树重建；新 extension 命名空间才使用已有初始流式构建器。精确旧 suffix 先删除再插入新 suffix。虚拟 PageId 归同一 MetadataWriteBatch，真实池/帧统一由最终协调器分配。
- 私有覆盖层在128 KiB固定前沿之外，每个实际加载/虚拟节点先计入64 KiB标准容器保守额度；原生键宽16/24/28字节受认证，codec/页镜像另按现有账户计费。额度在其治理的容器之后归还；这是分配请求计费，不代表完整进程 RSS 或完整成本验收。
- 15个受影响模块：默认302通过；候选437通过、1个条件跳过。覆盖混合降级、50→48保留列而48→47降级、清空所有成员后保留 live group、Node/Edge、只改实际字段页与目录路径、主Edge/邻接不变、NULL/-0/NaN位模式、逆转换、A/B恢复、预算不足及写帧前后取消/退款/重试。未运行全量单测或性能基准。

仍只涉及**原已在列中的成员**。缺失字段插入、既有 extension 更新/删除与晋升、通用值类型/大值、整 owner 删除及 Entity Map 联动、多组地图、搬迁和最终完整成本/同执行器 ROW 对照尚未完成；默认生产格式31未采用。**R3.2/R3.3/R5.1/R5.2/R5.3 未新增验收，总进度13/24（54.2%）；本轮不提交、不推送。**
