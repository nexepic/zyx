# R3.2 原生转换规划与私有 ID 准备

这是清单 base 上的未提交工作树检查点，不是已验收源码提交；未 commit／push，进度仍为13/24。源码补丁包含此前累积的私有图像／发布／绑定工作。

分类与反向投影现仅由原生存储 HybridPropertyGroupConversion 实现，使用原生记录和一次精确、持有 PMR owner 的有界成员／列状态及固定 workspace 分配；无默认分配回退，不虚构大值引用，不复制源堆字段。已认证且借用的 ROW 根须活到规划器释放。benchmark ReservedGroupConversion 仅转换实验视图，额外视图存储单独计费；存储层不包含 benchmark 或 query execution 头。原有转换／extension 语义测试文件与 base 字节完全一致。新增4项原生测试覆盖账户 owner 生命周期、85列／4080成员、超限／取消／退款／重试，以及借用 NULL／混型／堆字段的有序反向投影。

元数据私有 HybridPropertyConversionPreparation 仅允许 RelocatableMetadataStore／测试访问器构造；验证真实 batch generation，从同一个 MetadataWriteBatch 取得逻辑 PageId，打包真实类型化 group／column／inline-extension，并生成最终固定 slot locator／owner selector。不发布物理池／map／描述符／ID；失败丢弃整个私有 batch，不能另行发布 ID-only 预留。中性图像物理代际1须在后续绑定为实际池代际。测试的 metadata allocator 拒绝全部物理分配，只证明私有准备与 batch 计数，不代表真实图发布、A/B 故障或数据库旧快照。

5个必要模块默认33/33、实验38/38通过，无跳过／失败／禁用。实验测试覆盖类型化目录精确解析、4096成员／64列打包为11个实际 slotted 图像、源根／规划器释放后的准备数据及账户逃逸、陈旧／超大值拒绝，以及逻辑 ID 分配之后的分配失败／取消、完整缓冲退款和全新 batch 重试。测试预算保持既有1MiB上限。最终协调器仍须在 QUERY／WRITE 父账户下建立共享的有界转换子账户；任意 PMR 本身不向私有 helper 提供额度信息。

尚须：接入普通图事务、物理池放置、map修改／最终描述符、生产读写／索引／overlay、独立大值内容认证／归属／退役、反向转换／降级／删除、完整恢复／integrity／回收／复用／搬迁、完整实际原生成本及同执行器 ROW 对照。普通字段池与非空描述符保护继续保留。不声称正式性能／Kuzu、全套覆盖率、完整资源／RSS 或跨平台验收；固定R3／R5五项均未完成。
