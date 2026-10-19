# 未完成交付中的五项有界推进

父提交 `be10f801`，固定范围 R1.3/R2.3/R3.2/R4.3/R5.1。本轮完成下列五项登记的实现/验证推进，不等于验收五项阶段交付；总验收仍为 **13/24（54.2%）**。未跑全量单测或正式性能/Kuzu 窗口。

| 项目 | 实际变更和证据 | 剩余门禁 |
|---|---|---|
| R1.3 适配器 | 普通 Sort 的两处 key-slot 准入拒绝改为 MaterializedByteLimitExceeded；Top-K 按候选指针 vector 实际 capacity 对账。真实两行/32 key 查询在 4096 字节额度下拒绝，独立提高该次测试额度后精确返回排序值。该回归在父提交 runtime 源码上因旧普通 runtime_error 失败。 | R7 的其余消费者语义/性能复核；未添加自动重放 |
| R2.3 工作量 | LocationDirectory::findOptional 的根/内部下降复用已认证 unresolved radix 视图，保留类型/层级/可见性/选中子路由/叶检查。预热两层路由进行 10 命中+10 未命中：缓存固定复制计数 0，借用映像固定字节 324480，无 radix decode。 | R7 点查/扫描/gather 延迟和资源准入 |
| R3.2 转换 | ReservedFieldTransition 在纯内核前取得真实 MaterializedByteReservation，持有到结果结束，局部/父账户拒绝及内核错误都归还。实测保守 holder 加可能返回临时量预约为 3224 字节。 | 尚无原生 hybrid 格式/路由或 extension 发布；完整生命周期/成本/ROW 对照仍必需 |
| R4.3 维护 | 同次 registry 锁采样报告 pin 数及最老代际，精确排除观察者自身一个 pin。显式 compaction 观测增加最老 pin/代际差、退役队列记录数及最老退役代际，复用已有遍历。验证旧 pin、同代际多 pin、异域/已释放排除拒绝及搬迁/释放。 | 成熟写入、可复用/空闲/尾部观测及最终稳态读写/资源准入 |
| R5.1 一致性 | 最多 64 个引用的私有批次在暴露前核对身份、方向、顺序、continuation，并逐项与主 Edge 对照。结果拥有拓扑值并保留来源身份，禁止从临时 holder 取借用 span。 | 原生覆盖/分段生产者、原子生命周期、全局 integrity/退役及 R5.2/R5.3 生产/规划 |

## 实现边界

R1 修复资源分类正确性，不宣称排序提速。旧源码反向对照只把 LogicalPlanReadRuntime.cpp 替换为父提交版本，与当前支持库/测试链接；不是冻结的性能 before 二进制。保留 `reverse-sort-parent-failure.json`、`reverse-process.json`。主 runner 之后可用 `run-sort-regression-control.py` 复现该组件对照；不吞掉或重试 source pressure/取消异常。

R2 借用字节是包含所访问元数据类型的固定 decoded 映像大小，不是物理 IO、RSS 或精确 before/after memcpy 差。cache miss 移动/复制及其他路径不在断言范围。首次加载认证及缓存来源/失效继续由既有 reader 处理，没有新增缓存或提高上限。

R3 预约 sizeof(holder)+sizeof(FieldTransition)，覆盖没有 NRVO 时的返回重叠。同一父账户实际计费，预算必须比 holder 活得久。holder 禁止复制/移动，结果访问器禁止 rvalue，避免悬空借用。调用方 canonical extension 体及后续原生 map/allocator/发布缓冲须另外准入。封装由原生模块 runner 测试，但尚未连接生产属性写入，不能替代完整生命周期或估计器。

R4 的 pin 数/最老代际在同次 registry 锁下自洽；Catalog/退役队列属于单独 pin 的观测根，整个观测不是跨并发注册/发布的新事务快照。若 registry 最老代际比观测根新，代际差为零。代际差不是耗时；队列字段是记录数，不是字节或不同代际数；代际零表示空。观察者 pin 仍保护自己的读取，仅从报告压力中排除。既有显式观测本就遍历 Catalog/退役队列，本次没有为普通读取/提交加扫描，也没有为该观测新增遍历。不宣称空闲帧/尾部预算及成熟负载正式准入已完成。

R5 保留多重边、分方向自环，允许主 Edge 已支持的 type-token 范围（包括零）；拒绝重复/非递增 ID、陈旧/错误身份，logical continuation 提供精确下一 Edge 时核对首项。expected-first 为零表示初始/lower-bound 游标，不假定 ID 数值连续；已验证空终止批及满 64 项。原生生产者必须先认证磁盘路由并绑定实际 pin/store 来源，不能仅凭 database/generation 字段宣称来源正确。Lookup 必须在类型过滤之前使用既有主 Edge reader/记录复用；Lookup/guard 异常直接传播，不返回部分批、不自动重放。原生接入时仍须计费固定结果及返回临时量。这不是可绕过主记录的直接拓扑访问路径。

| 故障 | 必须在哪检测 |
|---|---|
| 已认证候选的 endpoint/type/Edge 身份错误，孤儿 lookup | 普通候选读取与主 Edge 对照，在类型过滤之前 |
| 重复/非递增 ID，错误 batch generation/direction/node，精确 continuation 不匹配 | 私有批验证，暴露结果之前 |
| 缺正/反向项、整项缺失、访问范围外孤儿/重复、陈旧磁盘 owner/slot | 原生写协议及完整 integrity；局部 helper 不能证明全局完整性，既有生产主 Edge 检查继续保留 |
| 验证部分前缀后或最后 checkpoint 取消 | 丢弃本地结果、异常传播一次，消费者看不到部分批 |

## 测试证据与范围

最终 `final-170.json`：**170/170**，17 个 suite、14 个必要源码模块。新增检查涉及原生点路由工作量、活跃 pin 排除、搬迁积压、分级转换预约/归还、真实 Sort 类型化拒绝/重试及候选拓扑/continuation/所有权；既有 coordinated budget、Top-K、location route/cache、snapshot/compaction 模块保护周边合同。首轮 `first-168.json` 一项 fixture 失败：把不完整合成 radix 文件交给 allocator 恢复，按预期拒绝非法 physical-slot tail。改为先对空且对齐的前缀构造不会用于写入的 allocator，再写合成只读帧；生产 allocator 校验没有改变。旧源码单例失败属于预期对照，与候选结果分开。

实际仅 Linux x86_64。新代码为可移植 C++20、枚举带前缀；不宣称 Windows/macOS、新一轮 sanitizer、完整绑定矩阵或性能结果通过。保留源码 hash、模块/suite/link 及零上下文源码 patch。配置 native Ninja/toolchain 后运行 `run-necessary-modules.py BUILD_DIR OUTPUT_DIR`；枚举完整目标命令不等于编译/运行全套单测。阶段验收及百分比仍仅由 optimization.mdx 管理。
