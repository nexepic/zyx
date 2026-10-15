# Overlay 与 undo 嵌套负载所有权

组件验证；进度仍为 22/34（64.7%）。

原生事务 overlay 现在在保留 Node/Edge 嵌套数据及复制语句 undo 前准入。所有权成员先声明，数据先销毁后退款。undo 条目原位构造；回滚直接交换租约、向量、字符串及标量元数据，不复制负载或构造临时 owner。原生事务回滚后，overlay 持有的数据继续占用 WRITE，直到 overlay 销毁。

替换工作区在 PMR 缓冲区中排序借用的字段/标签指针，构造标准容器前预付完整输出副本，移除了暂存专用中间属性 map 及重复负载复制。被替换缓冲区先释放，再吸收并裁剪输入额度；Edge 类型遵循相同规则。变更复制估算已移入格式无关的 MutationHeapBytes.hpp，由原生私有输入 owner 复用。没有 MaterializedByteBudget provider 的旧事务保留其既有分配合同。

103 项定向测试全部通过。新增用例覆盖嵌套 undo 准入失败、后续暂存拒绝后的 Node/Edge 恢复、成功重试，以及原生回滚后的 overlay 额度保留。新增分配观测覆盖 12 个 Node/Edge 替换、副本/undo 与交换阶段，未覆盖请求字节为零；五次原生暂存及另 22 种堆形状也通过。worker 共通过 579 个独立案例：572 个分配失败点、四个终态案例和三个堆计费案例。短路径穷举，长路径各采样 96 点。overlay 暂存正常分配数由 20 降为 18，overlay 创建由 24 降为 22；这些计数不是性能测量。

快照源物化、patch 规范化、便捷/API 副本及其他发布/编译工作区仍需补齐。不宣称完整堆计费、整阶段验收、Windows/macOS 执行或 Kuzu 性能结果。本组件未跑全量测试、新崩溃流程或性能基准；前一组件单独保留了 32 次崩溃恢复证据。

后续：规范化目标/键集合及排序描述符现已捕获 overlay WRITE owner。45 项相关测试通过，包括读取源数据前拒绝及退款后重试成功。此步只覆盖元数据；源与规范化负载副本仍待补齐。独立证明、源码与日志见 normalization-metadata-proof.json 和 normalization-metadata-evidence.tar.gz；此前 worker 仅对应其冻结源码。
