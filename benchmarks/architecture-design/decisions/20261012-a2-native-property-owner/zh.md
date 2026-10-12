# A2 原生属性解析的载荷所有权

日期：2026-10-12。

resolveProperties 原先先复制完整属性输入用于按键排序，再复制值到 resolved entries。现改为连续借用指针排序，只在生成拥有数据的 resolved entries 时复制输入值。输入键排序、空键/重复键拒绝、快照 token 解析顺序、token 排序及别名冲突检测保持原有合同。指针仅存在于同步函数调用期间，不逃逸到事务状态。

解析后的 ResolvedProperties 从临时对象移动到持久 Node/Edge 及创建 Node/Edge 的暂存状态，避免再复制完整 entries 与载荷。此处移动不改变调用方 const 输入，也不提前发布。已有模块用例覆盖四类目标、无效字段/token、原子失败、索引和约束检查、旧快照及向量索引更新；没有为了复制实现而增加冗余测试。

严格构建成功，NativeGraphMutationExecutorValidationTest、NativeScalarIndexMutationPlannerTest 与 ConstraintMutationValidatorTest 共 67 项定向测试通过。原始日志、JSON 和源码 hash 位于 results/20261012-a2-native-property-owner。仅验证 macOS；未运行 benchmark 或全量单元测试，未提交/推送，未更新正式文档或英文记录。

该调整减少两个位置的完整载荷复制，但仍未实现原生字段 patch 消费或局部物理属性写入。借用描述符数组仍由普通 allocator 分配，尚未接入统一预算。性能收益保持未验证，不据此验收 S1.2/S3.1。

进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
