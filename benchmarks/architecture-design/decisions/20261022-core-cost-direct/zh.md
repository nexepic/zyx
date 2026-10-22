# P5 直接类型化生产链路

实施进度 85%。263 项相关模块测试通过，未运行全量单测。一个未修改的遍历锚点优化器断言在冻结 5500a068 的头文件/库上同样失败，复现命令和原始日志单列，不计入本项通过数。

ROW 选读验证 owner、完整 offset/token 目录及选中 canonical envelope 后，同步传递数值位或字符串视图；数值直接入列，字符串只复制进查询账户持有的 offsets/bytes。每个 owner 的 optional-entry vector 和标量/文本 PropertyValue 不再创建。复杂值、LargeValue 和旧适配器保留具名 owning fallback；实际调用页校验不减少。借用仅在回调内有效，返回结果独立持有数据。没有新增持久格式、索引或缓存。

原生 pinned snapshot 明示 fence/schema/token 身份终身不变，DirectPropertyColumns/BatchEnvelope 在批边界核对完整身份，单元格仍检查批 owner/incarnation；mutable overlay 保留原有动态身份验证。短字符串过滤直接访问 offsets/bytes；预检后才消费，混合类型或超过 64 字节字符串退出该 kernel。每四个短字符串最多 256 字节工作一次取消检查，拷贝仍保留原 256 字节检查。实际 COUNT 先批内按逻辑字符串汇总，再合并全局分组；缺失/NULL 合并，空串及嵌入 NUL 保留，分组保持首次出现顺序。多键、DISTINCT、其他聚合/类型继续直接类型列通用消费者，未回到完整行 map。局部表占用现有账户，额度拒绝只在聚合状态未修改时退回；COUNT 合并检查 signed result 溢出，32 位平台也使用 uint64 状态。

medium 的一次只读机制检查：过滤/真实分组各 100000 typed text、零 owning fallback，196 批。过滤计数更新从 12500 次变为 196 次，真实分组从 100000 次变为 1568 次；oracle 分别为 12500 与八组各 12500。字段根选读仍是 100000 次，不把它写成已消除。

按计划另做同一 LogicalPlanReadRuntime 的 ROW/紧凑字段块边界原型：100000 个合成 owner、四字段、八个 country；两路均输出校验相同的实际过滤/分组结果。Release 源码 patch、静态库/探针 SHA 和全部三个样本已冻结。它仅隔离内存表示/解析，缺少真实目录、I/O、空间和更新生命周期，因此没有以它授权格式变更，也不用于与 Kuzu 比较。此前未完整冻结库身份的预实验不纳入验收时延。原型仍保留执行器成本，批级 COUNT 的工作量下降已在生产路径单独核验；最终时延只能由 P7 判断。

测试覆盖数值精确位、字符串借用/拥有生命周期、别名/重复、旧 pin、跨句柄增长/重开、混合类型、NULL/缺失、单/多键分组、完整 tuple、DISTINCT、空输入、溢出、取消、损坏目录、低预算与退款。P5 接入与机制通过不代表 5 倍收益或 Kuzu 目标已达成；P6/P7 继续，P7 门槛未修改。
