# A2 显式事务最终态到原生字段提交

日期：2026-10-13。状态：显式事务最终原生提交已接入，126 项限定模块测试通过；S3.1 仍进行中。

## 实现合同

事务仍用现有 overlay 和完整集合暂存来保持每条语句的 read-your-writes。最终 canonicalization 保留创建引用重映射、ID lease、创建后删除及最后一次完整替换的规则。在最终提交前，`extractFinalPropertyPatches` 将持久实体最终集合与事务自己的基底快照对照，生成真实 set/remove，交给上一轮的原生 token 消费者。调用方的变化字段提示不作为权威，不对新 generation 重放旧差量。

完整 map 替换仍删除未指定字段。含显式 NULL 的完整替换保留原始入口，因为持久 NULL 与 patch REMOVE 不等价。无变化的完整替换也保留原入口，以维持 intervening writer 的 graph fence 冲突检测。生成的 patch 具有 checked 累计 source/operation entry 上限；生成前有一次提前 fence 检查，发布锁内仍执行原来的权威检查。

这个最终态适配器仍需要完整基底属性读取、字段顺序描述符和临时差量；语句暂存尚未变为原生字段请求。额外读取/内存与减少名称解析/集合复制之间的净收益待最终统一性能对照。本轮没有以此取代 A6 的统一预算，也没有声明单字段局部物理写入或完成 S3.1 全部接口迁移。

## 原子性与恢复修复

- ordinary transaction 提交失败时，根据 backend 的 uncertain-publication 状态报告 `LGTTS_INDETERMINATE`，不再将可能可见的新 generation 报告为已回滚。普通发布前失败仍为 rolled back，fence 冲突仍为 conflicted。
- 事务先更新持久 Edge 属性、随后 DETACH DELETE 其端点时，最终 canonicalization 丢弃随该节点隐式删除的 Edge 属性变更。native writer 仍负责真实拓扑和 detach 上限验证；并未省略端点/边认证。

## 验证

只构建 production core 和 6 个相关测试源的对象，链接独立模块执行器。最终 4 个 GoogleTest suite 共 126 项通过、0 失败，涵盖：

- 多语句 Node/Edge 修改、16 KiB 大值、overlay 中间可见性、旧快照、真实 Cypher 显式事务与重开。
- 完整 map 替换、持久 NULL、无变化请求的外部 writer 冲突、创建引用与取消创建、早先有效语句不被失败语句破坏。
- 标量/复合/Edge 索引差量、未变化索引 root 保留、原生 patch 的约束失败和 vector delta。
- 属性更新的最终 Superblock sync 故障：事务明确为 indeterminate，原句柄拒绝继续写，重开后只接受完整旧/新属性对，integrity 检查通过。
- 既有普通写入的短写、磁盘不足与最终同步恢复回归；流式 bulk 模式不受此次普通 commit 分支修改，未列入本次选择。

本轮一次编译失败后脚本误运行了旧模块二进制，该结果不计。修复指针访问及脚本 fail-fast 后重新构建/链接，最终日志明确包含新增 Cypher、DETACH、NULL 和 indeterminate 测试；verification JSON 对这些成功用例做了核验。

验证环境为当前 Linux x86_64/C++20 云环境，不宣称 macOS/Windows 已构建或已完成真实进程崩溃/硬件故障测试。没有执行全量测试或 benchmark，模块耗时不是性能收益。

## 证据与下一步

`benchmarks/architecture-design/results/20261013-a2-transaction-final-patches/` 保存实际 build/test/link 命令和日志、来源/核心/执行器 SHA-256，以及 verification/progress JSON。正式 optimization.mdx 暂不更新。

下一步仍为字段请求暂存/消费者迁移和完整资源/故障验收。S0 25%，S1 14.3%，S2–S6 0%，总计 2/34（5.9%）。最新用户规则为验收百分比增长时 commit/push；本轮未新增已验收项，因此未提交或推送。
