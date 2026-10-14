# A1 绑定表示与物理计划设计合同

日期：2026-10-14。范围：S1.1 完整设计交付；生产算子迁移与性能归 S2/O1/O2/O6/O7。本合同确定第一版表示、算法、生命周期及替换边界，不把已通过的局部组件测试当作完整生产接入。

## 1. 当前调用链及确定的接入点

QueryEngine::buildPlan 从既有逻辑 PlanCache 取得 clone，或 parseToLogical → LogicalPlanOptimizer → QueryPlan。executePlan 仍由 LogicalPlanReadRuntime 的 executeOnSnapshot/visitOperator 执行。表达式与 mutation 读侧共用 evaluateExpressionWithSnapshot，公共结果在 QueryResult/Driver ABI 边界拥有化。

已实施基础：BoundSlotRegistry 的 scope/slot/plan 身份与 seal；多项投影的 BoundProjectionReferences；固定列/矩形批次；选列谓词及数值列；ExecutionSharedState/ExecutionTaskState；本轮的保守 bindingSchemaIdentity 和不可变 BoundPropertyBindings。前者不是完整绑定遍历，后两者没有启用通用物理执行器或缓存。join 当前已经用计费右侧哈希索引与共享绑定 key，不把它误述成无条件 nested loop。sort 使用拥有行与求值键的有界阻塞状态。

第一版新增边界固定在优化后的逻辑 IR 与执行实例之间：消费本查询拥有的 QueryPlan.root，形成 query-local sealed 物理描述符；不改 parser/逻辑语义，不把 task 状态写进逻辑缓存。移动已有查询 root 的所有权，不额外克隆每个 task 的 AST；原 const-root 调用入口仍要求 caller 保证执行期间不修改 AST。绑定描述符只暴露 const 视图，表达式引用的 owner 活到所有任务结束。

## 2. 目标内部接口与缓存选择

下表是下一实现的确定内部数据合同；现有实物类型在“依据”列标注。未实施的完整计划容器仍由 S2 接入，不将表格冒充已存在 API。所有表示为进程内，不规定持久 ABI/endian，不改变文件版本。

| 描述符/字段 | 定案 | 依据与约束 |
|---|---|---|
| program/slot/scope 身份 | uint64 program；uint32 slot/scope ordinal；不可复用、溢出拒绝 | 既有 BoundSlotId/BoundScopeId 与 registry；不以对象地址或 EntityId 当 slot |
| operator 表 | 计费连续表，uint32 operator/child/input/output ordinal，引用 program 内的 slot/layout；根 operator ordinal 独立保存 | 构造验证所有边、范围、非环与 arity，seal 后只读；无查询游标/行/参数值 |
| 表达式程序 | 有界连续指令表，输入/output slot、原语义 expression 身份、typed/dynamic 内核选择与 lazy/short-circuit 标记 | 保留原 evaluator/accumulator 的规则；不能每行重新遍历变量名；未知引用保留延后求值错误 |
| property 描述符 | BoundPropertyId(index:uint32, program:uint64)，source BoundSlotId、拥有 key、optional<uint32> token | BoundPropertyBindings 已实施；token 缺失选 name access，不能判 NULL/不存在；不推断永久字段类型 |
| 访问路径/算法 | 明确 scan/index/logical-index ID、read shape、join/aggregate/sort 选择及其限额来源 | 执行时按当前 snapshot 解析根与定义，描述符不缓存物理页地址；不可验证的索引访问转到显式有界 scan |
| pipeline 表 | uint32 pipeline ordinal、operator 范围/依赖、streaming/blocking 边界及允许输入/输出 layout | 首版单 task 非 JIT；legacy-row adapter 是明确节点，不允许未声明跳回旧行 |
| 参数 | plan 保存名字/ordinal及类型 guard 合同，query owner 保存本次值 | 参数参与专门化前验证；无参数值/本次结果进入 cached logical plan |
| schema/binding 身份 | 数据库/format、index-root-directory/page-directory 的 6 个 opaque token；overlay view/revision；能力校验 | 当前已实施的保守 observation，data COW 也可能失效；不是独立持久 schema epoch |

**第一版不启用物理计划缓存**。既有逻辑缓存只存逻辑 IR；每次 execute 在实际 snapshot 上绑定访问路径与 token。未来缓存只有在数据库/reader domain、语义 profile、绑定布局、能力、参数专门化 profile 及 schema 失效测试全部闭环后才启用。独立 schema epoch 若减少保守失效，须另行定义其 catalog 发布/恢复合同，不能把逻辑 root PageId/lifetime generation 或整个 graph fence 改名充当它。该选择关闭第一版 stale-token/cache 正确性问题，不声称缓存收益。

## 3. 绑定、值类型与语义错误

编译变量名到数值槽位；WITH 导出形成新的可见布局，隔离的子查询只导入声明的相关槽位，遮蔽不改父 slot。同名投影列 `n.rank` 优先于把它解释为 n 的属性，沿用 BoundProjectionReferences。未解析引用形成动态求值指令；不能在空输入时提前抛出原来不会发生的 row-dependent 错误。metadata 损坏、无效描述符及绑定预算失败仍传播，不能假装不支持优化。

property/schema token 在本查询的 authenticated observation 上解析；overlay revision 改变须重新绑定，同 revision 的不同 view 不相等。unsupported reader 不构造 token 表，选择显式计费的 name/evaluator 适配器；不伪造 schema identity。EntityRef 的 entity kind/ID 来自 query 的 data snapshot，schema identity 不代替实体读取/fence 身份。

固定内核首批仅 int64/double/bool。静态 literal 或已验证参数可选 typed 指令；无强类型 schema 的 property 仍是 dynamic，只有 producer 对实际整批类型/状态验证成功后才用同类型内核。混合数值、字符串/列表/映射/时间/bytes 走同一既有语义；不能因上批整数而接受本批浮点/字符串。NULL、missing 分开到存在性操作及输入边界，公共求值转换沿用原规则。AND/OR/CASE 的 short circuit 使用 active selection，不提前计算会报错的未选分支。integer SUM 溢出先检查，NaN/infinity/比较与浮点输入顺序复用现有实现；无分组空输入 COUNT=0，现有 SUM/AVG=NULL，不私自改为别的数据库惯例。

## 4. 物理批次 envelope 与动态 owner

第一版采用 **flat** 批次，header 明确 program/layout 身份、query data-snapshot owner 身份、uint64 batch owner/incarnation、uint32 physical row count 与 column count。batch 行数须同时满足配置、uint32 范围和可用字节预算，不能用 benchmark 固定尺寸做接口常量。selection 采用现有 uint64 bitmap；重复逻辑行保留不同位置，过滤不压缩/重排原位置。任何 append/reserve/truncate/clear/release 都终止已有 view 的借用；envelope 增加 incarnation 校验，不能保存旧 span 到下个 batch。

固定列复用 FixedWidthColumn/FixedWidthBatch：int64、double、uint8 bool，逐 cell 的 uint8 missing/NULL/value 状态。列拥有数组，实际 capacity/扩容重叠计费；矩形 append 完成所有列后发布行数。EntityRef 保留其 kind/ID，类型与普通整数不同；header 的 query snapshot owner/pin 负责解释，不借用页地址。

动态/变长目标 owner 定为 query-budget PMR arena：连续 byte buffer 及 descriptor/child-entry 表，PropertyType 的 uint8 tag、独立 cell state、uint64 offset/length/child ranges；string 与 bytes 不混淆，list 顺序与 map/function 的公开规则沿用原语义，日期/时间/时长保持原类型。offset checked-add、深度/元素/字节限额和 owner/incarnation 均在解引用前校验。owner 不能跨 budget move，view 不能跨复用；输出必须拥有化，不能用一个 scope 退出的 arena 充当 QueryResult。

此 arena 是 S2/A6 的实现目标，尚不存在完整生产实现。当前 PropertyValue/行及动态属性列适配显式保留；原 source、转换缓冲、临时 evaluator 值、selection 和结果全部列入 owner/计费清单，普通 vector 的 capacity 不等于逻辑 payload 长度。encoded-byte admission 也不等于全 allocator 字节。arena/decoder 尚未支持的类型必须选声明的 legacy value adapter，使用相同查询限额；不能在能力外静默创建不计费行。源 decoder、动态嵌套对象与所有固定 parent owner 的分配前准入必须在相应 S2/S5 资源验收前关闭，不以本设计宣称预算实现完成。


机器可读接口清单见同目录 `design-profile.json`。动态 descriptor 的 inline bits 保存 bool/int64/double/date/datetime（checked conversion/bit_cast）；duration 使用保留三个 int64 分量的 typed PMR 数组，list/map 使用有界 child/entry 表。所有类型均是进程内目标，UNKNOWN/COMPOSITE 不作为普通查询动态值。

## 5. 物理算子与阻塞边界

| 算子 | 第一版明确算法/表示 | 后续采用边界 |
|---|---|---|
| scan/filter/project | shaped scan 或已认证 index；flat 列/selection；固定同类型内核与动态语义回退；编译 slot/property 描述符 | producer 原生列输出后移除旧行输入适配；完整实体仅在表达式/输出明确需要时物化 |
| aggregate/DISTINCT | 既有 typed numeric accumulators 与原动态规则；完整可见 tuple 的 group/distinct key；计费 hash group，最后 takeResult | 不能各列独立去重；输入、group、结果并存计费；跨任务浮点合并未批准 |
| join/OPTIONAL | 保留当前共享绑定 key 的有界 build/probe 与候选后精确兼容检查；无共享绑定保持显式 Cartesian；OPTIONAL 对未匹配输入 NULL 扩展 | 迁移为列/引用候选不改变 multiplicity/key/collision；不能把相关分支当独立关系 |
| sort/Top-K/skip/limit | 现有比较/顺序合同；声明 blocking 的有界 row/key owner；只有可证明等价的排序+limit 才选 Top-K | group/repeat 边界不直接按 parent 截断；未支持 spill 时预算超限失败，不静默截断 |
| unwind/procedure/subquery/path | flat 初版，输出绑定 layout 与相关 import 明确；重复/调用次数/路径有序序列不变 | 每项列消费者迁移前保留显式计费 adapter；路径不能变节点集合或只保存端点 |
| mutation 读侧 | 使用同一绑定/表达式和 query snapshot；字段 patch/结果预检沿用 A2 | 绑定冲突要求整语句重算；不得在已发布后重放；durability 与 INDETERMINATE 不改变 |

## 6. 分组/因子化扩展的确定合同

envelope 的 layout discrimination 保留 flat/grouped 两种合同，第一版只允许 flat producer/consumer。grouped 扩展的 owner 包含父绑定列、uint64 child offsets（P+1，单调，末项=child count）、子列、correlation dependency/归属表、query/operator/group 身份及 uint64 positive multiplicity。group ID 不等于 EntityId；每个 child 明确属于一个 parent/相关组。checked-add/multiply 和 logical-work admission 限制展开及权重，不能仅按物理记录计数规避工作上限。

父字段共享只在同一个已证明相关组内；两个分支变量名字相同不构成独立性证明。无匹配 OPTIONAL 由语义 consumer 生成一次 NULL 扩展并保留父重复次数，空 child range 不是一个 NULL cell。路径保存同一条有序 node/edge 序列及重复身份。related subquery 输入 import 对应具体父组；有写入、volatile 函数、可观察调用次数/短路的操作按原重复次数展开。

| grouped 消费者候选 | 首版决定与实验准入 |
|---|---|
| parent-only filter/project | 仅纯且确定的已验证表达式可按组计算；子字段按所属 child；其它初版展开 |
| COUNT/SUM/AVG | COUNT 权重只有 logical row/cancel/overflow 与 NULL 规则等价后采用；SUM/AVG 首版按原序重复消费，不用浮点乘权或重排求和 |
| DISTINCT/group-by | key 是完整可见绑定 tuple；初版展开，不能独立去重各列再组合 |
| join/optional/subquery | 先验证相关依赖及 NULL/调用次数；初版 flat，不承诺所有 Cartesian 均可因子化 |
| path/sort/skip/limit/output | 初版有界、计费且保持顺序的展开器；输出逐个产生拥有行，不能输出借用 offset/pointer |

S2/O6 的代表性原型明确比较：单父多边、两个相关分支、真正独立分支、两/多跳、超级节点、空/NULL child、重复、自环/平行边、路径/ORDER/LIMIT/相关子查询与宽父值。记录 flat/grouped 元数据、复制/展开和真实分配/逻辑工作成本；正确性先以完整值/顺序/重复 oracle 验证。性能比较统一进入最终窗口；若不满足预登记主要/保护/资源门槛，保持 flat 并记录拒绝或修订，不强制采用分组，不把无连接扫描差距解释成因子化缺口。未执行此原型，不宣称已实现因子化。

## 7. 状态与生命周期

| owner | 状态 | 生命周期/销毁与首版约束 |
|---|---|---|
| immutable query program | query-owned IR、sealed layouts/instructions、算法/pipeline/输出名 | 不含 mutable task；所有任务结束后释放；物理 cache 关闭 |
| query shared | snapshot/pin、context/cancel、共同预算、已计费缓存、最终输出 reservation、全查询 work/首错 | 所有 batch/task/blocking/result 临时 owner 先结束，shared/budget 最后；当前 shared 容器非线程安全 |
| task local | 游标、批次/选择/arena、相关 injected input、累加器/统计 | Scope RAII 恢复输入与列视图；各 task 独立，不能共享可写 cursor 或 borrowed row |
| blocking operator shared | build side、group/sort/key、明确 merge/finalize 状态 | 计费且有界，所有 partial task 归并/释放后输出；不把 task aggregator 藏进 plan |
| public output | QueryResult/ABI 拥有的值/实体/路径 | 不借用 batch/旧页；public row callback 不做可观察 read-ahead，现有最终结果限定 read-ahead 边界保留 |

首版单线程；已有 shared/task 拆分不表示可以并发运行。O7 启用多 task 前必须协调全查询工作/内存准入、缓存访问、first-error/cancel、pin Reader 安全及 checked-add stats/稳定输出/浮点顺序；不能给每个 task 复制完整预算。compile/input/snapshot/type 或取消失败传播原错误，不 replay 已消费 aggregate/公开 callback；binding conflict 是明确重新绑定边界。计划/批次仅内存，无新 WAL/文件恢复/retirement；query pin 释放后仍由现有存储回收协议负责。spill 属 A6 的文件内执行，不由 A1 新建外部日志。

## 8. 显式适配与移除验收

| 当前适配点 | 可见职责 | 移除条件 |
|---|---|---|
| BoundProjectionReferences 旧 values map → slot | 编译一次名字、每行绑定借用、作用域清空 | 对应 producer 输出同一 sealed layout 的原生列；相同 projected-column/unknown-reference 语义回归通过 |
| 动态 property columns → PrimitiveNumericColumns/PrimitiveColumnFilter | 校验形状/类型与可选缓冲；无副作用前动态回退 | native owning column 与 typed consumer 同时接入，source+conversion 容量计费/guard通过 |
| LogicalPlanReadRow 与普通 evaluateExpression | complex/dynamic/未迁移算子统一语义桥 | 每个相关 AST/operator 的完整绑定与 typed/dynamic value accessor、错误/预算/取消测试通过；不得按一条快路径删除 |
| legacy snapshot/name access | 无 schema/token 能力的正确回退 | 有认证、同语义的 provider 能力后逐个替换；不强迫所有 backend 伪造 token |
| public QueryResult/ABI materialization | 受保护的拥有化输出 | 最后边界保留，内部优化只替换其输入；外部 ABI 不改 |

这些 adapter 在计划中有明确 layout/边界与 owner，不作为永久第二语义执行器。任何尚不能计费/正确保护的迁移路径不准进入对应 S2/S5 验收；缺少某算子不静默标为完成。

## 9. 模块清单、性能假设与反证

现有源码证据包括本轮 property/schema/overlay 的 103 项及重新构建的 A1 列/批次/槽位/投影/数值/谓词/选择/预算/嵌套执行模块；结果详见 acceptance-zh.md 与 results。只核验已存在组件，不代替未来完整编译器/arena/grouped 生命周期测试。未来必测：全 AST 作用域/参数类型变化、跨数据库/DDL/reopen/overlay 绑定、矩形/selection/incarnation、动态嵌套与完整实体/路径、所有迁移算子，空输入/NULL/重复/ORDER、实际预算/取消/early-stop、并发前的独立 task/merge 和拥有结果。

假设：槽位、解析一次的 property 与 typed batch 减少 hash/RTTI/每行装箱；代价为编译、key/descriptor 元数据、source/conversion 并存与动态 adapter。反证包括低行数编译成本更高、宽动态值重复复制、mixed/低预算频繁转换、分组维护/展开支配、旧行回退绕开准入，或浮点/错误时序/重复结果不同。不能以模块耗时宣称收益。最终同语义、同预算/durability、同结果消费的 control/current/Kuzu 窗口使用 optimization.mdx 第 6 节：主要 P50 目标至少 10%，保护 P50/P95 稳定退化不超 5%且区分控制波动，资源不提高；不支持或不等价 Kuzu 项记 N/A，不能承诺整体已超过 Kuzu。

本合同关闭 S1.1 的具体设计决定；S2 生产迁移、A6 完整资源、O7 并行及最终性能仍是独立未完成工作。正式 optimization.mdx 保留历史文本，当前状态以验收记录/JSON 为准。
