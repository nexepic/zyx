# S1.7 A7 设计逐项验收

日期：2026-10-13。结论：S1.7 设计交付通过；不改变 S1.1/S1.3–S1.6 或 S2–S6 的验收状态。

| optimization.mdx 的设计/发布要求 | 实际交付及复核 |
|---|---|
| 当前调用链 | zh.md §1；factory/probe/bootstrap/局部 reader 源码已核对，没有把 segment envelope 当文件头 |
| 目标接口、所有权 | §3、§5；root 注册合同、显式离线 source/target 及 private-build/output 归属明确 |
| 数据、快照与格式身份 | §1–3、§5；42 个显式版本常量来源、独立 namespace、owner/generation/route、目标独立 DatabaseId 与稳定 EntityId 规则 |
| 失败状态与拒绝 | §2、§5；codec/probe/select 的实际错误层次、split-brain、未访问损坏、失败/崩溃/indeterminate 发布明确 |
| wire/version 与兼容矩阵 | §1–3、version-profile.json；当前 4/30 与局部版本、flags 分支、不支持组合、未来每字段注册前提，不给未采用 layout 编造版本 |
| 恢复、回收与旧文件保护 | §3、§5；同文件 COW 可达、pin/retirement/integrity 责任；迁移源只读、新目标未验证不公开，仅清理自有目标 |
| 替换边界和稳定触发条件 | §3–5；无自动 in-place 迁移、当前 profile 未稳定、首次稳定格式与 ABI/库版本分离、发布前所有门禁明确 |
| 必要模块验证 | §6、results；103 项格式/codec/factory 模块通过，包含新增 checksum-valid 旧/未来 A/B 拒绝后文件字节/长度完全不变 |
| 成本假设与反证 | §7；无 runtime registry/提速声明，未来元数据与迁移资源准入明确，完整 integrity 不被采样替代 |

完整设计给出了当前事实、确定的后续实施合同及独立发布前提。计划明确要求本项交付稳定触发条件和版本边界，不要求本轮实现迁移器；因此这些发布前待实现项没有被误算为已实现能力。A3/A4 的 codec 定案/生产采用、A6 资源实现、真实迁移故障/跨平台测试均仍归原阶段。

新增验收 ID 为 S1.7。固定 34 项：S0 1/4（25%）；S1 2/7（28.6%）；S2–S6 0%；总计 **3/34（8.8%）**。S3.1 的原生 consumer/显式事务及暂存折叠是实现进展，未据此关闭完整字段迁移/验收。正式 optimization.mdx 表格暂不更新，来源与进度以此验收记录及 JSON 为证。

依据最新用户规则，百分比增长后执行相关优化工作 checkpoint 的提交和推送；执行结果记录在 progress/交付消息，不能把本页当作已推送证明。未运行全量测试或 benchmark。
