# A2 约束增量核对、复制消除与按键接口边界

日期：2026-10-12。任务：S1.2/S3.1 的局部推进。状态：当前源码 30 个相关套件共 505 项通过。完整属性按键协议仍未接入，不能将本轮声明为 A2 完成。

## 实际源码发现和改动

ConstraintMutationValidator 的受影响实体路径已经比较前后规范键；未变化键不进行支持树候选扫描，也不产生支持树 mutation。完整图验证路径同样跳过相同键。因此保留既有机制，没有另造一套“约束增量优化”。约束根存在性、NODE KEY 非空、真实规范键重复、原子键交换、碰撞上限及 stale-entry 错误仍由既有协议保证。

持久节点更新原来通过 value_or 复制标签向量，并为新属性集合再复制一次向量。本轮改为借用 mutation 中的新集合或已验证旧记录，owner 均覆盖本次规划使用。旧/新权威属性根、事务持有的请求仍保持原拥有化协议，不向外暴露该局部借用。唯一约束候选校验复用一次 keyFor 的返回值，避免同一候选的重复编码；错误 kind/ID 时保持原短路行为，不提前编码损坏身份。

补充真实原生执行器断言：保持唯一字段 email 不变而修改 note 时，支持树根 PageId/generation 不变，实体新属性可见，旧快照仍保留旧属性。随后原用例继续验证唯一字段变化和删除。初版 4 KiB 载荷超出该夹具 inline 上限且未配置 LargeValue placement，既有错误正确拒绝；本测试目的为支持树不变，已使用普通 note 字段，不修改生产限制或宣称大值路径验证。

## 后续按键接口的明确边界

当前 LogicalGraphPropertyMutationRequest、LogicalGraphMutationBatch、overlay 和 NativeGraphMutationExecutor 仍以完整集合替换为持久入口。LogicalPlanMutationRuntime 的单字段 SET、map 合并和 REMOVE 先维护完整 PropertyMap，再输出完整替换。现行完整替换请求必须保留，不能将其默认含义改成 merge。当前支持的 Cypher 动作以实际 parser/runtime 校验为准，内部完整替换能力不等于本轮新增 Cypher 语法。

后续按键请求采用独立请求种类，不在既有 properties 字段中隐含操作模式。目标字段合同为：entity 沿用 entity_kind/reference_kind/value 的既有宽度与身份；sets 是拥有数据的 LogicalGraphWriteProperty 列表；removals 是拥有数据的键列表。单请求中键非空、各列表内无重复、两列表无交集；sets 仅包含非 NULL 值，Cypher SET NULL 在调用侧转为 removals。removals 对缺失键为无变化，且只解析已有 token，不为不存在字段分配 catalog token。该类型和 batch 消费尚未实现，不能进入现有后端被静默忽略。

| 来源 | 归一化行为 | 顺序与可见性 |
|---|---|---|
| 单字段 SET | 仅提交最终 changed key 的 set/remove | RHS 按现有语句顺序求值，后续表达式看到当前值 |
| map 合并 | map 中各字段转为 set/remove，其他字段保留 | 不把合并当整集合替换 |
| 完整集合替换 | 使用明确旧请求，省略字段删除 | 后续 patch 以替换后的集合为基准 |
| REMOVE | 显式 remove | 缺失无变化，不创建 token |
| 同事务重复修改 | 最终请求可折叠同键，逐条 overlay 仍及时更新 | 不能在 RHS 求值之前用最终状态替代中间状态 |
| create 后修改 | 在 provisional 身份上修改最终创建集合 | 保留现有 ID lease、create/delete 折叠和 read-your-writes |
| delete 后修改 | 沿用已有删除状态拒绝 | 不复活实体，不吞掉引用错误 |

一个最终 batch 对同一实体不同时携带完整替换和 patch；有序语句由 normalizer 产生单一最终请求。多个 statements 的语义与最终持久请求不同，不能用 batch 列表遍历顺序猜测语句先后。未支持 patch 的后端必须显式拒绝或经已声明、计费的完整替换适配器转换，不能只增加一个 vector 后让现有消费者忽略它。

## 原子、快照与资源合同

字段变更、派生标量/约束/向量状态、实体及 catalog 仍在单个 COW 发布点生效。计划选择的快照 fence 保留，错误、唯一冲突和损坏不能因 patch 绕过验证。overlay 在语句失败时恢复该语句前状态；最终提交失败或不确定提交沿用现行事务状态，而非宣称已回滚。稳定 ID 与 owner 校验不变。

新值必须拥有数据；未修改字段若后续布局允许借用 LargeValue 引用，必须绑定同一快照 owner/pin 并在生命周期结束前完成解析。当前普通请求仍携带完整 PropertyValue，不能提前声称已经避免所有大值解码或实现局部持久写。旧布局适配成本与未来 A3 页级 COW 成本分别验证。

按键请求准入必须计入实际 vector/string/value 容量、归一化状态、约束候选及峰值复制；查询预算、backend 资源限额和 A6 总协调不得各自漏计。取消/byte admission 失败不得发表局部结果或重放先前修改。新增协议需补齐节点/关系 SET/REMOVE、map merge/replace、同字段多次修改、NULL/缺失、值未变化、类型变化、索引键碰撞、创建删除、旧快照、重开和故障发布模块测试后才能准入。

## 证据及限制

505 项选定模块测试全部通过，覆盖约束的 26 项测试、原生执行器与标量索引，以及既有执行绑定/批次/查询模块；不是全量库测试。定向日志保留的是初次测试状态，含夹具大载荷失败；最终 modules-tests.json/log 为修正夹具输入后的通过证据，不覆盖或隐藏初次失败。构建继续引用现有依赖缓存，无安装。

证据目录：benchmarks/architecture-design/results/20261012-a2-constraint-copy/，包含当前源码 hash、模块配置与日志/JSON。本轮没有持久格式变更、新恢复信息或新的回收 owner。测试 fixture 删除自己创建的临时数据库；增量构建缓存为后续模块测试保留。macOS 本机验证，不宣称 Linux/Windows 已验证。

性能假设为减少规划复制与候选编码；未运行 benchmark，不声明已测性能收益。未执行全量单元测试、提交或推送，未更新正式数据库文档或英文历史记录；新增代码和注释为英文。S0 25%，S1–S6 0%，总体 1/34（2.9%），按键请求的端到端生产接入仍待继续。
