# R3.2：真实候选持久提交与重开

本轮优先打通实际持久链路，而非再增加独立规划器。阶段验收仍13/24（54.2%）：完整双向写入、回收及对照未完成，不提交/推送。未跑全量单测或新增性能窗口。

## 已接通

实验格式32中的显式 `publish_candidate` 转换请求从原生事务或类型化元数据入口进入现有 writer。来源依旧由元数据在同一数据库/源代数 pin 下自行读取，调用方不能提供权威来源、PageId、物理位置或规划回调。默认请求保留此前的无写入准备探针；默认生产格式31仍关闭此功能。

候选将真实 group/column/extension 字段页和最终固定目录 B+Tree 页写入由同批次保留的物理池，并在同一个最终 A/B 发布中登记 allocator、Catalog、Page Directory、Index Root Directory、ROW 权威删除和非空描述符。物理页先于可恢复的元数据根。共享页写入口只有内部字段模式接受 POK_HYBRID_PROPERTY，普通 standalone 分配/替换及普通描述符发布仍拒绝该路径。

来源 ROW 页也按真实 PageId 合并 COW：仅删除本次转换 owner 的 root slot，保留同页其他 owner 的字节、slot locator 与逻辑 PageId，再把清理后的 ROW 页映射到新物理帧。没有通过仅删目录键来留下当前物理孤儿。旧 pin 继续路由到旧 ROW 帧；全局审计在新 pin 上只看到保留的 ROW root 和新字段。清空后的 ROW 页目前仍映射保留，不宣称已完成回收或空间优化。

非空描述符的实验读取已启用，字段根仍与同一 pin 的实际 Index Root Directory/namespace 和字段实体身份认证；读取不会失败后退回 ROW。描述符形状拒绝缺少 group/owner 根的组合。全局 RelocatableIntegrityScanner 现同时核对 hybrid 四目录和每个当前物理字段页的精确反向归属，继续检查剩余 ROW slot 与目录的一一关系；原生字段体不进入全库原始页副本集合。显式受保护 scanner 账户保持其原始所有者；仅无账户的 legacy 调用保留原有默认资源约定。

当前候选持久提交后是**只读原型**。在 writer 锁内、任何写入/回收前检查当前非空布局，拒绝未适配的后续写入；最终发布另需元数据私有批授权，不能借 generic 根发布绕过。普通默认格式 writer 保留原来无 admission 回调的直接路径。后续需要将完整字段修改/逆转换/退休接入专用 writer 后，才能逐步解除只读限制。

新建独立 LargeValue 仍在写文件前拒绝，避免缺少内容索引/全局回收生命周期却发布新对象。已存在、已认证的独立单字段 LargeValue 可以复用，其 manifest/content index 保持原映射。旧 ROW carrier/旧数据池保留的所有空间必须在后续成本与退休准入中计入。

## 验证

相关11个模块：默认构建220项通过；实验构建323项通过、1项格式条件跳过。最终原始JSON/日志及源码、二进制哈希随附，未执行全量单测。新增集成验证涵盖：

- 稠密列和稀疏 extension 的真实提交、字段值/成员计数、同代 Entity Map 与 ROW 权威切换、独立只读重开、旧 pin；
- A/B 部分写回到 ROW、完整 inactive 写选择非空候选，描述符/目录/ROW 权威始终匹配；
- 写出一个物理候选池后取消，来源根不变，退款且重新分配后重试成功；
- 来源 ROW 页中 owner65 保持原始 locator/值，group1 转换不影响它；propertyless live group 不建立空字段树；
- 全局完整性扫描、合法 CRC 的 namespace 根替换拒绝和账户退款、修复后重新核对；
- 新 LargeValue 在数据写入前拒绝、独立大字段复用及完整内容/manifest 审计；
- 既有 snapshot/共享池/元数据拒绝/public transaction/完整性损坏模块。

初轮构建发现 MetadataWriteGuard 不可移动及 shared append 拥有标准 vector，已分别用 writer 锁内私有 admission 入口和预付费的 append payload 修正；失败日志保留。只验证 Linux x86_64，不冒充其他平台或完整成本/性能验收。最后的小幅调整保留默认 writer 原快路径，并仅在有描述符时构造 hybrid scanner 状态，最终重跑相关模块。

## 未完成

完整逆转换与后续单字段/多字段/whole-owner 写入发布；多个已转换组的增量共享 map 修改；新 LargeValue 内容索引、全局 ROW/native 引用合并及安全退休；旧 ROW 页/物理帧/池的回收与复用；完整成本及同执行器 ROW 对照；生产 R3.3 和 R5.1–R5.3 原合同。R3.2 尚不能验收，性能/Kuzu 最终窗口仍按用户要求等待全部实现完成。
