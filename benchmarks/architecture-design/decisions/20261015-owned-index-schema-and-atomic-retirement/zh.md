# Canonical schema ownership and atomic retirement

索引定义的名称、属性数组和 metric 自持额度，目录结果数组接入已准入 backing，缓存复制明确选择 CACHE，释放私有查询所有者。编解码先用固定大小借用视图验证全部字节及语义，再创建有额度的字段；重复属性校验不再分配集合。宽目录在 32 KiB 测试预算下明确拒绝；向量恢复每周期仅点查一个路由，完整定义/根校验保留在启动路径。训练按规范名称点查，避免复制无关定义。

并发维护复现“检查没有快照后新快照注册”的竞态，导致底层安全拒绝被上层当作永久故障。新增原子 try-release：在同一 metadata writer guard 内验证记录并检查可见快照，只有快照压力返回空结果，错误代次/损坏/I/O 异常继续抛出；严格旧接口仍报错。Slotted/Entity/physical relocation 的维护协调器据此延迟重试，成功回收才失效空闲空间缓存。原并发断言和等待上限保留，增加失败诊断。

1278 项相关模块测试通过；故障矩阵 885 个案例通过，含 32 轮崩溃/只读哈希/恢复。原竞态修复后连续 10 次通过；缓存复制 10 个分配故障点穷举通过，其他长路径最多抽样 96 点。六种编解码/缓存堆形状未发现漏计，释放后存储和额度归零；这些形状不声称全部可作为原生持久 B+Tree 条目。旧计费测试由 capacity+1 改为真正堆请求（SSO 不另记字符堆），保留严格等值/释放断言；属性容量测试改用受控 reserveString，未降低上限测试。

仍须闭环 B+Tree 原始 key/临时数组、原生调用缺少显式数据额度的路径、其他编译/运行工作区、维护队列及稳态验收。

Component evidence only:22/34 (64.7%). Linux/clang19/libstdc++14; no macOS/Windows runtime, RSS or new Kuzu/performance benefit claimed.
