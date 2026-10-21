### 2026-10-20 R3.2／R4.3：发布前完整实配门禁、ROW 保留与恢复

证据：`benchmarks/architecture-design/decisions/20261020-r32-prepublication-allocation-admission/`。显式格式32正向转换可附带 allocation_admission：源与完全准备好的私有候选均由元数据协调器核对持久allocator／Catalog／退役／reserve，固定至少省10%的字节门禁，并核对准入峰值／文件high-water阈值。调用方不能提交伪造成本凭据；成本未知、已登记的可选census额度不足、收益不足或超过准入阈值时保留原ROW根／ID／值；损坏、取消和I/O错误继续传播。

统一发布拆为私有最终metadata／route准备与A/B发布，准备只执行一次。私有页先刷出cache，再按稳定epoch观察，后续发布不再分配漏计metadata池。这是发布前的完整实配判断：会发生私有COW／flush及放弃后的恢复I/O，不是无需I/O的预测器；仍是显式实验入口，不是生产切换。准入阈值不替代原WRITE额度与StorageIO硬文件上限，私有准备也不能绕过原硬上限。

拒绝的候选可能已向当前ROW内容索引池追加未发布帧；即使没有hybrid描述符，下次普通ROW或仅metadata发布以及可写重开也先恢复。只在真正遇到共享数据池时预约恢复frontier，空metadata库不多占该工作区。修复并保留启动恢复的既有FERR_INVALID_STATE分类，三个损坏拒绝测试未放宽。

新增12项门禁／恢复检查：未知成本无写入保留ROW、稠密小形状字节拒绝、峰值与文件阈值、实际StorageIO硬上限、完整准备后的取消、缓存dirty页清空、ROW内容索引失败后的普通写／metadata-only重开恢复，以及相同已恢复源文件的私有计费与真实发布后分配完全一致。最终仅运行37个相关模块：默认528通过；候选787通过、1个条件跳过。未跑全量单测或性能窗口，仅验证Linux。

完整map事务、schema／统计／查询索引／未知State及通用拓扑生命周期、同执行器查询对照、生产准入及R5／R7仍待完成。**总计13/24（54.2%）；没有阶段增长，不提交、不推送。**
