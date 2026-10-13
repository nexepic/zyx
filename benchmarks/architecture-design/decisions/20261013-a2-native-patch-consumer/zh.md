# A2 原生 token 属性 patch 消费者

日期：2026-10-13。状态：真实 backend 与隐式 Cypher 写入已接入，59 项模块测试通过；S3.1 仍进行中，S3.5 未验收。

## 实现与边界

组合 patch 入口成为内部虚函数。RelocatableStorageBackend 校验来源 snapshot/fence，将请求交给原有发布锁、冲突检查及不确定提交处理。NativeGraphMutationExecutor 在自己的 fenced transaction 内读取 token 属性根，只解析 SET 字段；REMOVE 只查已有 token，不为缺失名称创建 token。source、sets 与 removals 在同一事务内合并，未修改载荷移动到最终 token 集合。创建 ordinal 在本批次内折叠进创建字段。完整 replacement 与 patch 的引用冲突、重复/空键、NULL set、删除目标、entry 工作量上限在发布前拒绝。

源属性按逻辑身份顺序读取，同一 NativeReadSession 认证其 snapshot。源值解码沿用 encoded-byte admission；entry 工作量上限与全查询 allocator 预算不是同一指标。创建输入的字段数量在复制/解析前准入；持久 source 的 entry 数在读取后检查。批次排序、合并及字段解析保持取消检查。尚未完成统一资源预算，不能据此验收 A6。

属性、scalar 索引、vector semantic delta、标签和统计仍经过现有 native 发布协议。约束检查的临时 final-state view 只转换该实体种类的约束字段；权威 root 始终保留全部属性。没有约束定义时，不创建第二份拥有整批创建/替换载荷的 constraint batch。创建边的 resolved override 和属性载荷直接转移到最终 root，避免重复解析与复制。

本轮不改变持久 codec、公开 Driver ABI 或 COW durability。旧物理布局仍完整编码最终属性集合；没有声称单字段 SET 只写单字段。显式事务的 overlay 继续使用原适配器，其字段请求暂存、最终 native 提交以及故障恢复仍需迁移/验收。没有启用全消费者原生批次能力，也没有删除动态适配器。

## 验证

当前 Linux x86_64 云环境使用既有 clang-19/C++20 构建配置。只重建 production core 与受影响测试对象，并链接包含 3 个模块 suite 的 GoogleTest 执行器。59 项通过、0 失败，覆盖：

- 真实 Cypher SET/REMOVE 经 trace 确认进入 native patch 消费者，RETURN/未变化字段/删除字段正确。
- Node/Edge token 合并、创建 ordinal、未变化字段不重新解析名称、精确 entry 上限及无 fence/引用冲突拒绝。
- 旧 snapshot、16 KiB LargeValue source、重开、admission 拒绝后重试、唯一约束的多目标与 companion 标签原子失败。
- 约束所需字段的 transient view 不解析无关 1 KiB 字符串名称，NOT NULL 失败不发布。
- vector 字段 patch 与图根同一发布，旧 snapshot 仍能读旧值；既有 native mutation 和 overlay patch 回归全部通过。
- 对两目标乱序 patch，在每个实际规划 checkpoint 依次注入取消，逐次验证两目标均保留旧值；正常重试成功。

1 KiB 字符串验证使用不配置 LargeValue 的低层 fixture；真实大值由 backend 的 16 KiB 测试覆盖。未执行全量测试、性能 benchmark、commit 或 push；不以模块执行耗时宣称提速，不宣称其他平台已构建。

## 证据与进度

证据位于 `benchmarks/architecture-design/results/20261013-a2-native-patch-consumer/`：build/test 日志、模块 link 命令、源码及二进制 SHA-256、verification 与 progress JSON。正式 optimization.mdx 暂不修改。

保持固定 34 项：S0 1/4（25%），S1 1/7（14.3%），S2–S6 0%；总计 2/34（5.9%）。本轮完成的是 S3.1 内的 native 直接消费者，不替代完整显式事务/故障测试或性能验收。下一步继续事务字段暂存、最终态折叠与原生提交。
