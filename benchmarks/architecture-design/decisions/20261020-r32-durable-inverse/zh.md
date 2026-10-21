# R3.2：持久逆转换、退休和复用

本轮接通真实 hybrid→ROW 发布，并修复最大组的前向转换账户峰值。验收仍13/24（54.2%），不提交/推送；没有全量单测或新增性能/Kuzu窗口。

## 已完成的链路

内部类型化转换方向增加 ROW_TO_HYBRID / HYBRID_TO_ROW（枚举均有 Windows 安全前缀）。实验格式32中的显式逆转换通过同一个原生事务和精确来源 fence 进入 writer；只有这一受控路线获准在已安装的非空布局上取 writer，普通未适配写入/回收仍被拒绝。

元数据在 writer 下认证实际 native 来源，复用已有有账户的 HybridPropertyRowPreparation 物化当前 ROW 值。当前持久原型限定为描述符中唯一的已转换组/namespace，完整枚举 group/column/extension 定位和真实 B+Tree 退休计划，并核对每个当前物理字段记录的反向归属。每个树计划有128页上限、逐页取消检查和预付费的256 KiB保守标准容器 scratch。4096是本次逆转换的成员工作量上限，不是 group 终生容量。

新 ROW root 共享页/池，使用同批次 PageId 和物理保留。单独的大字段可复用原有、已认证的单字段 LargeValue reference，不分配新 Value ID；其他超大 ROW carrier 的协调尚未安装，在写盘前明确拒绝。普通 inline ROW 编码仍使用既有 canonical codec，NULL、负零和 NaN payload 原始位在实际往返中保留。

新 ROW 页先写盘，再在一次 A/B 发布中登记 allocator/Catalog/Page Directory/ROW owner roots，移除 native 固定树根及字段/树页路由，清除描述符 root，并把旧候选池与 Retire Queue 一起提交。退休前逐池核对最多31个实际已分配帧及当前 point route，拒绝移除其他 current frame，无需为这一步扫描全图 Page Directory。退休池的物理帧不提前覆盖；旧 pin 仍通过旧 Catalog/目录读取 native 字段。恢复 ROW 路由后，既有普通写入和回收可继续。

清除 descriptor 不降级文件格式：文件仍是实验格式32，不自动变为生产格式31。未采用生产布局。

## 实际资源问题及修复

新增最大组测试最初在前向真实提交处超过1 MiB，而此前仅私有准备通过。原因是解码后的 ROW 根、其 decode admission 和转换分类工作区一直保留到物理/目录发布。现把读取/分类/字段编码放入独立生命周期；字段图像及 LargeValue proposal 已拥有自己的编码字节，输入值和分类工作区在后续 index/physical 规划前销毁，再退款。头部计数的临时 decode scope 也先结束。没有提高1 MiB限额。

64 owner / 4096成员组现能在原转换账户内完成 ROW→hybrid→ROW 两次真实提交，验证新/旧 pin 值和全局完整性。这里验证的是转换账户，不代表所有元数据/底层通用 writer 的完整进程内存或成本估算已经验收。

## 验证

相关12个模块（非全量）：默认246项通过；实验359项通过、1项格式条件跳过。包括之前的发布/读取/描述符/页池/snapshot/全局损坏验证，以及既有 B+Tree 原子树退休26项。新增验证覆盖：

- 原生事务逆转换、恢复普通 ROW 更新、真实 writable 重开；
- 部分/完整 inactive A/B 中断分别恢复完整 native/ROW 状态；
- native 旧 pin 阻止退休池释放，释放后普通页以不同物理代际复用相同 slot，旧定位不别名；
- 单独大字段复用且 next_large_value_id 不增长，propertyless 组往返；
- 额度拒绝、私有准备后取消、已写 ROW 物理页后取消，来源根保持不变并退款重试；
- 未支持的大 ROW carrier 在写盘前拒绝，global integrity 仍通过；
- 实际 NULL/负零/NaN payload 往返及最大组1 MiB双向持久提交。

失败证据保留：测试误用不存在的 visitPageLocations API 已改为既有 visitPageLocationsUntil；浮点 fixture 的 ROW owner 配置903与 seed902不一致，被既有 stale/foreign 检查正确拒绝，已修正配置；最大组1 MiB真实失败和存储生命周期修复后的通过证据均保留。没有放宽来源验证或扩大限额。只验证Linux x86_64。

原始JSON/日志、精确链接参数、源码/二进制哈希及相对9cc2821c可重建的完整工作区代码补丁随附。进度不提前计数。

## 尚缺

已安装 native 布局的单字段/多字段/whole-owner 实际修改发布；多个转换组的增量共享 map；新独立 LargeValue 与通用 ROW carrier 的内容索引/引用协调；前向清理后空 ROW 页和旧池的完整退休/稳态复用；完整成本、同执行器ROW对照及 R7；R3.3生产准入和 R5.1–R5.3原合同。仅双向原型提交和回收测试不能完成这些阶段。
