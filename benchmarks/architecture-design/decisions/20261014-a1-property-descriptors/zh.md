# A1 不可变属性绑定描述符

日期：2026-10-14。状态：私有执行层的属性描述符组件及真实 snapshot 边界完成模块验证；完整 S1.1/S2 仍未验收。没有把它启用为跨查询缓存或另一个查询执行器。

实现为 `src/query/execution/BoundPropertyBindings.hpp`。输入由封闭的 BoundSlotRegistry 数值槽位和字段名组成。构造时校验所有 slot 的 plan/index 与字段工作上限，再解析 token；同一 slot/key 的重复请求复用一个描述符，不把不同变量槽位混为一项。请求映射和唯一描述符均有自己的 PMR 容器，字段名由组件拥有，不借用 AST、输入字符串或行。

`BoundPropertyId` 的 uint32 index/uint64 program 区分描述符归属；每个 program 获得不复用的进程内 ID，溢出拒绝。同一槽位 layout 的不同绑定表以及原地址重建的表也不能接受旧描述符。sealed layout 只在构造时借用；组件复制来源 slot ID，不持有 registry/snapshot/page 指针。查询预算必须比组件活得更久；组件不可复制/移动，防止内部 memory resource 地址失效。

来源身份使用上一记录的保守 catalog/page-directory 视图，另记录 supportsPropertyTokenResolution 能力。构造完成前再次检查身份/能力，避免 resolver 期间的视图变化发布混合描述符。使用 view 前检查 program、ordinal、来源身份和能力；变化抛 BoundPropertyBindingConflict，需要对实际执行快照重新绑定，不能自动重放已消费的行。schema 身份不是 data value cache 或完整物理计划 cache key。

解析出的 token 必须非零。缺少 token 时，BoundPropertyView 同时保留拥有字段名的借用视图，并要求按名称访问；它不是逻辑 NULL/缺失证明。真实覆盖层中新字段在提交前可以存在而没有持久 token，因此这个回退是必要语义。组件不推断字段永久类型；泛型 map、实体和动态值的访问选择仍归后续物理消费者。

所有 descriptor/source/key 输入在 token resolution 前验证。maximumProperties 限制原始请求数，maximumKeyBytes 限制原始字段字节工作量，包括重复请求；这些是工作上限，不是 schema 字段数。PMR 准入另外计入实际 vector 容量、字段字符缓冲以及去重临时 map 节点，临时查找结构在构造完成前释放。预算不足、allocator 失败、取消、无效 token、无效身份和 source 错误不吞成 token 缺失；构造失败返还组件的容量，已有 query owner 保留其预算。固定对象与整个查询/decoder 的统一资源闭环仍归 A6，不宣称全部资源已统一。

模块验证包含 11 项新组件测试、12 项既有 slot registry、6 项真实 schema/descriptor backend 及 74 项覆盖层测试，共 103 项、0 失败。覆盖重复/不同 slot、输入字符串销毁后字段名仍有效、无 token/无 token 能力、foreign/越界/stale program ID、所有 catalog token/域/revision/能力变化、resolver 中身份变化、零 token/source 异常、无效请求先拒绝、每个实际 allocator 分配点失败后重试、共享预算压力、每个构造检查点取消与容量返还。

真实 backend 中，新 catalog 视图拒绝旧描述符，原 pinned source 仍允许使用并读到旧值。真实 overlay 修改后旧表失效；重新绑定没有持久 token 的新字段仍可按名称读到 9，rollback 后来源读失效。这里只验证组件与来源边界，没有宣称普通查询管线已迁移到此组件。

证据位于 `benchmarks/architecture-design/results/20261014-a1-property-descriptors/`，包含源码 hash、模块构建日志、链接命令及原始测试结果。当前 Linux/C++20 验证，未运行全量测试或 benchmark。源码基底仍为已推送 646298ae，后续 A1 源码/记录暂留工作树。当前验收 S0 25%、S1 28.6%、S2 0%、S3 20%、S4–S6 0%，总体 4/34（11.8%）。后续须完成通用物理描述符/表达式绑定、batch envelope 与动态 owner、分组/消费者和显式适配移除合同，才能验收完整 S1.1。
