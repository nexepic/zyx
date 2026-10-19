# 下一组五项：消费者适配器与认证元数据复用

范围固定为 R1.3、R2.1、R2.2、R2.3、R3.1，起点源码 27d93bc4，见 registration.json。R1.3/R2.3 完成本阶段实现与证据后仍待 R7 性能准入，不能提前计作已验收。R3.1 只交付编码前设计，不切换生产格式。

## R1.3 消费者登记

普通原生扫描、字面量过滤、真实聚合继续使用 R1.2 的 DirectPropertyColumns。下表登记其余已有有界适配器，不把“输入曾经过批”说成零复制。逃逸输出均拥有值；源缓冲、算子保留行与最终输出分别纳入现有查询预算。取消/消费失败不得作为源配额分裂而重放。

| 消费者/触发条件 | 实际所有权边界 | 上限与成本观察 | 退出适配器的条件 |
|---|---|---|---|
| Top-K/Sort | visitBoundedTopKSort、visitSpillableSort 或有界内存排序；tryVisitBatchedPropertyExpressions 经 ChargedRowVisitor 交付 owning 行与排序键 | 行数/字节上限；Top-K 保留 K 并计费候选；普通排序保留文件内 ExternalQuerySort spill。sort.topk_input_rows、topk_retained_rows、sort.spilled、consumer_adapter.sort.calls | 完整排序键、稳定 ordinal 平局规则、owning 输出及相同 spill 协议 |
| Join/笛卡尔积 | 绑定输出名、规范化 owning 行、有界 rightRows 与计费 hash 键/候选 ordinal | 右侧行上限和共享字节账户；不宣称 hash-join spill。consumer_adapter.join.calls | 重复次数、共享变量相等性和 NULL 等价；完整 build-side 计费 |
| Expand/变长/OPTIONAL | 现有拓扑批页和 continuation，owning 行合并；gather 恢复 ordinal；保留主 Edge 认证 | 拓扑/源/候选/输出预约与取消；consumer_adapter.expand、variable_expand、optional 的 .calls | 端点/type/overlay/续读、自环、多重边、OPTIONAL 补 NULL 全等价 |
| UNWIND | 绑定表达式在 owning 输入行求值，元素和输出拥有数据，保留重复 | 现有行/输出/元素配额；consumer_adapter.unwind.calls | 列表顺序、每行相关性、生命周期等价 |
| CALL 子查询 | 注入 owning 行、现有绑定程序与查询局部状态 | 共享行/字节上限及取消；consumer_adapter.subquery.calls | 相关性、unit 子查询基数和副作用不重放 |
| Procedure | 绑定输入导入、ProcedurePayloadAdmissionScope 帮助函数、owning 输出 | 同一资源协调；bound_inputs.imported_programs 与现有 payload 准入/失败证据，不把调用次数说成结果行数 | ABI/绑定和 procedure 特定 payload 语义全部支持 |
| Mutation 读侧 | executeWithSnapshotAndInputRow 按 overlay/fence 刷新，owning cache 转移预约 | 现有源/消费者/路由预约；暂存变更后刷新值 | mutation epoch/overlay 失效和副作用单次求值证明 |
| CSV/UNION/DISTINCT | CSV 配置 import root 与 owning 行；UNION 保留重复策略；DISTINCT 延用已支持的内部 scratch | 同一资源上限；consumer_adapter.csv、union 的 .calls；不宣称通用外存执行 | URI 权限、类型/NULL/重复及当前 DISTINCT spill 等价 |
| 一般 Project、残余过滤、复杂聚合表达式、不支持直接单元的 provider | 现有绑定表达式输入和计费的属性批转行适配器 | logical_plan.batch.flushes/rows/materialized_bytes 是累计量，不是峰值内存；索引候选仍做残余过滤 | 直接求值完整支持表达式、错误和所有权；普通 R1.2 路径不得整体回退 |

实现仍由 src/query/execution/LogicalPlanReadRuntime.cpp 及既有 payload 帮助函数持有。新增 consumer_adapter 指标在实际 dispatch 处每次调用计数；嵌套/相关子查询可重复，因此不是去重计划节点数或行数，也不声称覆盖绕开 visitOperator 的 procedure/mutation dispatch。instrumentation 不引入回调副作用。

## R2.1 实际调用链

| 路径 | 实际链路 | 已有复用与重复工作 |
|---|---|---|
| 选字段点读 | LogicalGraphReadSnapshot → PageBackedLogicalGraphReadAdapter → token/NativeReadSession → PropertyCollectionRootStore → EntityProperty Directory → Page Directory/catalog → 认证属性 slot → 选列 codec/可选 LargeValue | 已有快照绑定 token/entity/property cache 和 MetadataPageReadContext；点读校验访问到的 owner/slot generation/payload，不做无关全局检查 |
| 有序扫描 | DirectPropertyColumns → 属性扫描游标 → LocationDirectoryRangeCursor → unresolved radix/认证子路由/leaf → propertyPageOrder → 批内不同页各取得一次 → 逐 owner 选列解码 | 单调范围共享目录流，范围重启重下降；最多保留最后一张属性页。leaf 已借用，缓存 radix 仍复制固定数组 |
| 邻居属性 gather | 拓扑页 → 源 ID 排序去重 → 原生属性适配器/定向排序定位或扫描游标 → 页序读取 → 原 ordinal/重复位置恢复 | 现有 sorted multi-lookup 按请求 ID 缩小子路由；并非所有 gather 都走该方法，属性游标仍可能走范围流；数组与输出继续计费 |

R2 新增 readMetadataRadixUnresolvedView，只接入既有 location range cursor 的 root/internal 读取。命中共享 const 解码页；未命中 inline 持有现有解码结果；零预算、未来/未发布页继续无缓存。子路由解析只复制所需 RouteFrame 引用，不修改共享 unresolved 页。目录类型、root/internal kind、深度、可见 generation/LSN、环、leaf ordinal、owner 检查全部保留。

缓存仍绑定 StorageIO/数据库快照、物理 slot/frame/generation、codec 类型，路由项再含逻辑 ID/root/fence。未新增缺失缓存、重叠缓存、额度或格式；不删除文件大小检查。同 IO 写 epoch 使查找缓存失效；其他合法句柄 COW 发布不修改旧 pin 的不可变页，新快照使用自己的上下文。搬迁和物理复用保留稳定 PageId 与 generation。已借出页靠共享 owner 存活，即使 lookup 失效也保持旧内容；后续读取重新加载认证。缓存命中不承诺每次发现外部介质破坏，由后续认证 reload/完整 integrity 发现，合同不变。

## R2.3 工作量证据口径

decoded_metadata_copy_fixed_bytes 统计成功缓存值拷贝的 sizeof(Page)，也含路由缓存值拷贝。decoded_metadata_borrow_fixed_bytes 统计共享视图代表的 sizeof(Page)。均为累计固定结构字节，不含嵌套 vector、返回 move、RouteFrame 复制或 cache miss codec 拷贝；不是物理 I/O/RSS。原有命中、解码、页、file-size、checksum、I/O 指标独立保留；既有有界 PerfTrace page-pin 区分 generation/context 镜像；唯一物理 offset 需另行去重，不能用命中次数代替。嵌套 timer 不相加冒充耗时。

定向模块测试将小型精确值 fixture 的点读两次、扫描两次、定向位置 gather 两次写入原始 JSON，证明机制和单位，不是完整规模 P50/P95。十次 radix 命中对照单独验证拷贝/借用字节和零新增 codec 解码。历史 c287 完整规模诊断仍仅作 R0 归因参考，不冒充新源码 after。R7 继续按原预算与保护场景统一测点读/扫描/gather、写入和稳态资源，最终性能准入未完成。

## R3.1

详细且已关闭参数的候选见 r3-layout-zh.md / r3-layout-en.md。本组不含原型、codec、格式版本升级或生产切换。

## 必做跨句柄测试发现并修复的失效问题

首轮 103 项有 1 项失败（102 项通过）：另一个可写句柄使文件增长后，新快照读取报 shared page-pool header 与 Page Directory 不一致。分段断言定位到新快照，旧 pin 仍返回原值。根因是帧分配 control record 可变但 pool incarnation generation 不变；另一句柄 positioned write 不会使本句柄 raw cache 失效，旧 (pool offset,pool generation) 键可能返回认证合法却陈旧的 bitmap，看不到新分配帧。

复制和 pinned 两种 known-location 读取现将**控制页镜像**键绑定为 (pool offset,快照发布 generation)，明确包含解析/可见性上下文；解码后仍按实际 catalog pool generation 验证 envelope/header。不可变数据页仍使用 (offset,实际物理 generation)。同句柄 control 写已有按 offset 失效所有镜像的机制；外部发布后新快照不复用旧 control 镜像，旧 pin 的旧映射仍有效。不全局清缓存，不加缓存/额度，不删 file-size/认证/owner/allocated-frame 校验，不允许回收 pinned 帧。

不同时代的控制镜像可能在原 buffer pool 上限内共存，新发布 generation 的首次读取也可能增加工作；R2.3/R7 必须保留资源/点读保护。此为正确性修复，不宣称未经测量的提速。原生测试覆盖双句柄、实际文件增长、旧/新精确选字段值和重开；既有 pinned 文件压力/复用、页池搬迁及故障模块保护生命周期。保留首轮失败原始证据。

跨快照诊断中 PerfTrace page-pin 身份含所给 generation/context，同一 offset 的不同镜像会区分；报告唯一物理 offset 必须另按 offset 去重，不能把镜像数说成唯一设备页数。

## 必要验证与实际机制计数

12 模块检查运行 182 项，173 通过；9 项属性 cache fixture 预期“控制元数据已热、只读一张冷数据页”，但 setup 只预热 writer 旧键，未预热新的快照上下文读键，实际多读 4096 字节控制镜像。修正 setup 的显式控制页预热身份，保留所有“一张数据页/miss/resident”断言不变；未为这 9 项修改生产代码或放宽断言。重跑该模块全部 21 项及此前未选中的相关子查询/惰性表达式用例，22/22 通过。两轮合计 183 个不同用例的最新状态均通过，不是一次 183/183。提交首轮真实跨句柄失败的 103 项 JSON、182 项结果、最终 22 项及派生逐用例最新状态；没有全量单测。

182 项中的生产源码已经通过所选 Top-K、NULL/NaN/溢出、重复/自环/多重边、owning 输出、sort/DISTINCT/UNION spill、allocator/搬迁/复用与发布故障检查；最终只改 fixture，不使这些生产验证失效。实际运行 Linux x86_64，不声称 macOS/Windows 已执行。

以下为小 fixture 各两次调用的累计量。控制元数据显式预热，同快照先点读，再 scan，再仅位置 gather，不是相同冷态对照或 before/after 延迟。

| 路径 | 固定元数据复制字节 | 固定元数据借用字节 | 选中 owner 解码 | file-size 调用 |
|---|---:|---:|---:|---:|
| 点读×2 | 72720 | 496 | 2 | 4 |
| 扫描×2 | 28288 | 16472 | 4 | 2 |
| 定向位置 gather×2 | 0 | 16304 | 0 | 0 |

位置 gather 不含属性解码/I/O，不能当完整属性查询比较。剩余复制量证明只读 radix 视图没有消除全部元数据复制。此 fixture scan requested-child=488，定向位置 gather=2：范围游标会提前解析剩余范围，定向方法按请求缩小子项；已有路由目标复用与子项选择工作需区分。这不是新加 cache 或宣称全部工作仅随选中页增长的理由；R2.3 完整规模/性能复核仍待准入。

复现时激活工具链后运行 run-necessary-modules.py BUILD_DIR OUTPUT_DIR。脚本仅枚举总测试 link 命令，实际编译/链接所列对象和 core，再按显式 filter 执行。
