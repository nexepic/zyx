# A2 属性增量修改：完整设计决策

日期：2026-10-12。范围：S1.2 的设计合同。实现与性能验收分别归 S3.1、S3.5，本记录不以设计取代实现。

2026-10-14 接口细化与 S3.1 实现验收见 `../20261014-a2-field-staging/zh.md` 及 `acceptance-zh.md`。后续采用独立 owning raw 暂存入口与默认关闭的事务能力，替代本文第 2.1 节拟增的 batch 字段；保留全部消费者迁移与字段语义要求。本文其余“尚未实现”等表述保留为当时状态。

## 1. 当前调用链和问题

Cypher SET/REMOVE、MERGE、CREATE/FOREACH 在 LogicalPlanMutationRuntime 中读取语句快照，维护实体最终属性与别名可见值。支持 shaped admission 的快照产生 LogicalGraphPropertyPatchRequest，否则使用旧完整替换请求。补丁经 LogicalGraphWriteBackend 的组合适配器读取源、排序合并成 property_replacements，和 companion batch 一起交给 executeLogicalGraphMutationBatchWithReadControl。

显式事务通过 LogicalGraphTransactionOverlay 先 preview，再 apply/stage；异常恢复 overlay，成功递增逻辑 revision。最终提交由实际事务负责。直接原生路径由 NativeGraphMutationExecutor 解析 token，构造新旧 root、约束及索引状态，在 NativeGraphWriteTransaction 的同一个元数据发布点提交。当前补丁仍经过完整属性集合，字段接口变小不等于物理写入变小。

## 2. 采用的接口与替换边界

采用已有内部 LogicalGraphPropertyPatchRequest：entity 全引用、sets 拥有键与非 NULL 值、removals 拥有键。完整属性替换继续作为独立操作。字段键非空，单请求内唯一，sets/removals 不相交；单批次每个全引用最多一个补丁。完整替换与补丁不得命中相同全引用，调用方按语句顺序折叠后选择一种。

组合入口必须显式接收源快照、planning fence、累计 entry 工作上限以及同步 read control；创建 ordinal 只在同一 companion batch 内有效。不得把相同数值的持久 ID 与创建 ordinal 混为一谈。存储接口不用公开 Driver ABI 的新增函数，当前 ABI、文件 codec 和页类型保持现状。

第一阶段保留旧布局适配器作为语义正确的通路。下一阶段在内部 batch 增加字段操作后，必须同时迁移批次空判定、shape 验证、preview/stage、overlay、创建引用重映射、约束、native executor 和测试 backend；不能仅迁移 native 单一路径。迁移完成后，支持原生字段消费的 backend 不再经 query 层全集合替换；其余 backend 的适配器保留明确能力边界。不得并存两套独立 Cypher 语义。原生字段消费属于后续实现，尚未启用。

### 2.1 原生批次接入的确定合同

下一阶段在内部 LogicalGraphMutationBatch 尾部增加 property_patches（vector<LogicalGraphPropertyPatchRequest>）与 maximum_property_patch_entries（size_t，默认 0）。非空 patch 列表要求正工作上限，含源字段及 sets/removals；空列表不额外触发写入。内部 C++ 结构调整不增加持久 codec 或公开 Driver ABI 函数。既有组合入口保持签名，以 backend 能力选择原生消费或旧布局归一化；能力接口定为 supportsLogicalGraphPropertyPatchBatch()，默认返回 false，只有消费者清单全部迁移且定向测试通过后启用。

native shape 验证对补丁和完整替换共享全引用冲突集合，先验证删除/创建引用与工作上限，再做源读取。sets 用事务快照及同一 catalog session 解析/创建 token；removals 查询该 session 已有 token，未存在的名称按删除不存在字段处理，不为删除动作创造 token。先拒绝不同名称解析到同一 token，再按 token 合并 old root 与操作，保持一份权威最终 root。没有布局切换前允许最终 root 完整编码，不能以原生消费宣称局部物理写入。

创建引用直接折叠到创建 properties；持久引用按逻辑 ID 排序读取。标签与属性先构造同一实体最终态，再一次计算 scalar/vector/constraint/statistics 变化。原生消费者不得在源解析前取新 generation 重放，schema/catalog 身份与 planning fence 同时验证。约束扫描仍在实际 backend 验证全部最终态；显式删除与 patch 冲突立即拒绝，DETACH 的隐式删除关系由拓扑最终态验证负责。

overlay 在 stage 时顺序应用补丁，对 preview IDs、逻辑 revision、异常还原与 read-your-writes 使用同一合同；提交前可以归并多次字段变化，但不能抹掉中间语句可见性。适配器退出要求 runtime、overlay、transaction preview/stage、native executor、索引/约束与 fixture 都消费同一字段合同，并完成真实 backend 语义及故障测试。这是下一阶段实现规格，不表示结构字段或能力分支已经落地。

## 3. 语句语义和最终态

单字段 SET、map 合并、完整 map 替换及 REMOVE 分别保持现有公开语义。map 表达式完整求值后再应用，不能从 CASE/嵌套表达式抽出第一个 map literal。NULL 赋值规范为删除，未指定字段在补丁中保持；完整替换移除不在结果 map 中的字段。

同一实体的所有变量别名及后续 FOREACH 迭代读取当前暂存属性。多个语句仍按顺序展示 read-your-writes，只在最终提交折叠持久状态。创建后删除消去最终实体；创建后更新合入创建载荷，避免无意义的二次写入。删除目标不得额外执行属性修改。恢复原值允许得到空补丁；数值类型及 NaN 依据现有 PropertyValue 比较合同处理，不额外发明宽松等价关系。

## 4. 数据身份、所有权与预算

持久引用以 entity kind、reference kind、ID 组成身份。planning fence 必须来自执行读取的快照；overlay fence 同时包含基底与逻辑 revision，不能仅验证 durable generation。快照 pin 覆盖读取、解析、规划和提交验证。

补丁与 companion batch 拥有输入载荷；排序描述符和 string_view/PV 指针只在同步函数中借用，不逃逸。resolved entries 拥有解析后的值，转交暂存状态采用移动。源 payload 在合并时移动给完整替换，只复制变化输入值。输出拥有自己的数据，不借用提交后失效的 entity cache 或旧页地址。

entry 上限是源字段与操作累计工作量，不是 allocator 字节上限。现有 shaped admission 约束编码源字节；局部 PMR 查找 owner 约束自身实际分配。A6 必须将输入、源、补丁输出、resolved 状态、结果及写事务私有资源纳入共享预算；禁止把多个独立上限相加后声称查询总预算成立。当前尚未闭环，不以本设计宣布资源验收通过。预算/取消失败必须在发布前传播，不作为可选性能回退吞掉。

## 5. 索引、约束与统计

采用 canonical old/new 索引键差量。canonical key 与索引成员身份相同则跳过，删除/增加/变化分别执行必要操作；标签变化也影响部分索引成员，不能只比较属性值。索引编码包含值类型，必须保留其现有碰撞比较协议。唯一性候选、范围支持及约束继续观察最终批次状态；不能因接口是 patch 就绕过跨实体验证。

Node 与 Edge 的已支持索引能力分别沿用现有 codec；不得为了对称性启用尚不支持的 Edge composite。向量属性变化仍经同一事务的 semantic delta，不能脱离图数据独立发布。统计按真实成员增删计算；无变化字段不无条件删建索引。所有派生状态与用户图状态同一 generation 可见。

## 6. 失败状态和重试

无效引用/重复键/冲突操作在读取与 staging 前拒绝；源缺失、源损坏、token 别名、约束/唯一性、预算及取消失败均不得部分发布。前一实体成功归一化不允许先行提交。RETURN/ORDER BY 所需的语义与资源预检必须在发布前完成；创建真实 ID 的最终输出沿用既有创建身份协议。

fence 冲突要求重新读取并重新执行整条语句，不能在新 generation 重放旧行集或旧差量。有限重试耗尽明确返回冲突。发布前失败可以回滚；若最终 durability fence 报错而发布可能可见，沿用 INDETERMINATE 合同，关闭/重开核验，不能报告已回滚或自动重复提交。

## 7. 字节布局、恢复与回收

A2 第一阶段不增加持久字段或页格式，PropertyCollectionRootStore 继续生成完整 replacement payload/encoded root。页内 COW 与 LargeValue 仍可能重编码整集合，因此不承诺单字段修改只写单字段。

LargeValue 复用只能使用 snapshot 验证的不可变引用，沿用 reused_large_values 与新 large_values 的不同处理，不允许借用过期物理地址。根、目录、LargeValue 与索引在同一 native metadata batch 可达；旧快照仍引用旧版本，retirement/回收沿用现有协议。A2 不独立引入 GC。改变属性物理粒度必须由 A3 定义权威版本、格式、故障发布、integrity walker 与回收，不在字段接口中暗改字节意义。

## 8. 模块验证清单和现状

现有定向测试涵盖：补丁保留字段、乱序输入、累积 entry 上限、显式旧 fence、取消/read-only、混合 Node/Edge、源 admission 失败、创建 ordinal/持久 ID 分离、创建补丁/完整替换冲突、companion 标签原子处理；Cypher map 四种语义、最终态、别名与 FOREACH 连续读取；native 索引根保留、约束冲突、旧快照、向量更新及解析取消。

本轮上述相关模块共 292 项通过。原始日志/JSON 和当前调用链源码 hash 在 results/20261012-a2-design-contract。采用现有严格模块构建；未运行全量单元测试。原生字段消费接入后还须增加其真实 backend 路径的故障发布、重开、旧快照、LargeValue 复用与回收测试，不能用适配器测试替代它。跨平台验证和最终资源/性能验证仍待后续阶段。

## 9. 性能假设与反证

假设：折叠最终状态、避免未改变载荷复制与索引重建，可降低小更新 CPU、暂存内存和索引写入；旧布局的完整集合编码仍可能占主导。最终测试分别记录源读取、解析/合并、索引变化、属性编码、发布的成本，以及真实分配/读写字节、RSS、空间和 P50/P95；阶段计时不得与正式 benchmark 混跑。

保护场景包括小 owner、宽 owner、大值、混合类型、同值更新、密集索引、批量和逐条更新、长快照及失败回滚。若主要场景未达到文档门槛，或保护场景稳定退化超阈值，则不能宣称性能验收通过；若完整集合编码主导，应推进 A3，而非重复包装 patch API。最终同语义/同资源与控制源码及 Kuzu 对照；当前不运行 benchmark，不提前填收益。

## 10. 交付判断

本记录覆盖 A2 的调用链、目标接口、所有权、身份、失败、格式/恢复/回收边界、替换条件、测试清单及性能假设。设计合同已具体化；原生消费者、统一资源 owner 与物理布局实现仍未完成。S1.2 的逐项复核见 20261012-s1-2-design-acceptance/zh.md；该复核仅验收设计交付，不新增实现或性能验收。正式 optimization.mdx 按用户要求暂不更新。

最新设计验收后的进度见 20261012-s1-2-design-acceptance/zh.md；原 1/34 是本记录初稿时状态。未提交/推送，未修改正式数据库文档或英文记录。
