# S1.6 A6 完整设计交付验收

日期：2026-10-14。结论：S1.6 设计交付通过；S5 的全运行时资源治理、单文件查询 spill、并行及最终性能均未完成。合同为同目录 zh.md、en.md、design-profile.json；不是把基础组件通过测试记为全执行器完成。

| optimization.mdx 设计要求 | 决策与当前证据 |
|---|---|
| process/database 总限额及并发查询归属 | 预算层级：有限总额、category caps、query/account 共享；7 项 foundation 测试包含两个真实并发线程/多个 DB 和总额拒绝 |
| cache/write/maintenance/code 与 query 唯一计费 | 所有权表定义真实容量/重叠/嵌套/fixed owner、共享 borrower、未开启 code cap、输出活过 close 与 keepalive；尚未迁移对象明确阻止 S5.1 |
| 子预算申请/归还、转移与失败原子性 | 同一 root mutex 原子检查三层；同 account split/absorb 不扩大额度，PMR upstream 前准入/bad_alloc 退款；size_t 边界已测试 |
| 压力次序与死锁规则 | 无资源等待，释放 optional、驱逐 unpinned、支持且有工作区才 spill、一次重试后失败；resource/metadata 不嵌套，禁止 mutex 内回调/IO |
| 取消释放、RSS 和外部开销 | owner/task/buffer/lease/pin 销毁顺序、原始错误与 orphan backlog；实际 global requests 和 local estimates/RSS 分列，不承诺 RSS 硬限 |
| 单文件 scratch 权限与 durable owner | 专用内部 permit、物理只读失败、独立 SGK_QUERY_SCRATCH、持久非复用 owner/high-water、认证 manifest/location 与五态发布/释放协议 |
| 活查询 pin、共享句柄与恢复 | 独立 owner/generation pin、共享 metadata 旧 pin 保持、exclusive startup 前认证 orphan 清理、错误登记不回收用户页、不用 bulk cleanup 处理 query scratch |
| allocator 与用户提交互不覆盖 | 同一短 publication gate、最新 allocator 状态重建、保存既有 write fence；A5 后续处理保守冲突，不在资源设计中降低隔离 |
| spill 顺序、算法及不支持项 | sort run/stable ordinal/bounded merge 优先，再 canonical partition/hash；skew/depth/IO/disk 失败具体；COLLECT/DISTINCT/浮点顺序及未合格 codec 明确 gate |
| 长快照、维护预算及观测 | maintenance cap/保护余量/64-step 轮次、有限 retirement backlog、压力失败与可观察指标，禁止无界增长/破坏 pin |
| 格式、测试与性能替换门 | A7 format gate 后才赋 query kind/codec，当前无 disk/ABI 变化；S5.1–5/O7/O8 fault/correctness/resource 清单具体；最终性能反证与 Kuzu 窗口明确 |

必要模块：ResourceCoordinator+A1 当前组合 82 项与相关 runtime 222 项，重叠 26，**278 项独立测试通过**。7 项新 foundation 测试只证明三层 counter/cap/transfer/allocator 并发基础，不证明完整生产接入、slot/floor、keepalive 或 spill。Linux/C++20；无全量测试、性能基准或跨平台实测声明。原始日志/链接命令/源码 hash 在同名 results 目录。

固定 34 项新增 S1.6：S0 1/4（25%）；S1 4/7（57.1%）；S2 0/5；S3 1/5（20%）；S4 0/4；S5 0/5；S6 0/4。总体 **6/34（17.6%）**。current-progress.json 同步，optimization.mdx 的历史表不改。设计进度增长按用户授权提交推送；完整优化仍在推进。
