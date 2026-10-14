# 查询 scratch 格式登记——实施草案

状态：存储与排序正在接入，**S5.3/S5.4 未验收**。全局资源所有权、可分区哈希执行、恢复/故障覆盖及最终对照仍待完成。权威台账保持 22/34。

开发期格式从 envelope 4/Superblock 30 切换到 envelope 5/Superblock 31，新增 `SGK_QUERY_SCRATCH = 15`。旧文件应拒绝且不写入，不迁移，不从 A/B 回退到旧 profile；其他局部 codec 版本不变。查询数据只登记于认证的 sealed Catalog 条目，不进入公开图目录。session owner 使用已有持久 Node ID 高水位分配，未使用的 ID 为允许的间隙，不是存活 Node，因此重开不重用 owner。

LogicalQueryScratchProvider 内部权限与图 mutation 权限分离。逻辑只读查询只能在原生可写文件上获取，物理只读文件无此能力，不暴露宿主临时路径。多个 session 共享同一后端的独占文件 scratch lease，外来可写重开不回收活动 scratch；物理只读重开不清理。bulk 恢复只选 kind 14，query 恢复只选 kind 15、逐 owner 清理。清理前认证 envelope/Catalog 身份及 payload CRC。

不透明 handle 为新分配的全局 SegmentId，只在本 session 映射认证的代际/owner。限制活动片段数及已分配私有 segment 字节；IO 前预留登记条目，避免成功发布后因分配失败丢失清理路由。读取返回独立字节，不将私有 reader pin 暴露到调用外。释放必须先停止私有读取，核对 owner/kind/generation、拒绝公开 Page/Extent 路由，原子标记 Catalog obsolete 并释放 allocator bit。普通图快照保持图/元数据历史，但不阻塞私有查询数据槽释放。清理不确定则要求重开，不猜测用户页可丢弃；有活动 session 时禁止关闭后端。

Row v1 的 24 字节头为 u64 magic、u64 输入序号、u32 binding 数及 u32 排序键数。后接有序 binding（u32 名称长度、名称、u8 kind）：kind 1 为 canonical PropertyValue；kind 2 为五个 u64（实体 ID、edge 0/1、端点及类型 token）；排序键为自定界 canonical 值。不存指针、物理位置、借用 owner 或行缓存；延迟实体/属性读取仍使用原查询快照。完整实体/path 暂不支持 spill，内存不足明确失败。计数在 reserve 前按实际 frame 长度验证。

排序块 v1 的 16 字节头为 u64 magic、u32 行数及零 u32，后接 u32 行长度与 Row v1；外层原生段校验身份与完整 payload checksum。获准行最大编码 4 KiB，排序键不含 NaN；不支持行不会触发源重放。输入序号保留稳定 ties，NULL 顺序沿用原比较器；游标验证宽度、顺序和基数，二路归并最多 12 层。行内容、实际 PMR vector 容量、路由表及保守 codec/IO workspace 有本地准入。发布前即时检查，常规比较使用普通协作守卫；唯一一行保留惰性 ORDER BY。全局内存/磁盘上限尚未接入，不能从本地上限推断完成。

已有 bulk decoder 也在 reserve 前按 wire 字节限制计数，避免损坏字段申请几十亿项；这是定向单 bit 破坏测试发现的正确性问题，与新格式独立。

容量审计补充：新外部排序缓冲显式启用实际 PMR 容量与嵌入对象计费。对旧通用缓冲的整体切换引发低预算回归，这些原有路径暂保留载荷计费，等待全局 domain 接入后继续审计。这是尚未完成的资源门槛，不是验收通过，也不是放宽基准预算的理由。

查询 scratch 在准入未完成期间默认关闭，原生测试显式开启。provider 暴露是否启用，因此默认路径与原生物理只读路径保持已有内存排序。每查询默认磁盘额度为 128 MiB/1024 片段，与登记目标一致。

清理工作续进：单片释放和批量 finish 在发布或 I/O 失败后立即保护后端，保留原始异常与重开恢复所需的清理路由。新增双模式故障测试验证后续写入被拒绝、旧图快照可读、析构释放会话配额以及重开完整性。本次不验收 S5.4，其完整故障矩阵仍待完成。
