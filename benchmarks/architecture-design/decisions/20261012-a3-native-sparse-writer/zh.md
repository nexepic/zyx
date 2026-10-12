# A3 原生稀疏组原子写载体

日期：2026-10-12。

新增 NativeSparseGroupWriter 实验载体，复用 mutateNativeGraphPagesWithLargeValues，将变更列、新 descriptor 与内容索引放入一次原子 metadata publication。要求同 metadata/LargeValue/property store 域、明确源快照 fence，以及候选目录身份恰好为下一 generation。未修改列比较不可变字节后复用引用，并核对 hash/长度、当前快照 manifest；引用按逻辑 ID 去重纳入 derived reused_large_values。新逻辑 ID 由源快照 next_large_value_id 计划，现有 metadata writer 负责原子预约/发布。

真实原生测试：首次两列加 descriptor 分配三个值，仅推进一次 generation；单字段更新只新增目标列与 descriptor，共两个值，另一列引用不变。新组读到新值，旧快照读到旧值。候选 generation 错误、过期源快照以及 descriptor 字节上限失败均拒绝，generation 和 next_large_value_id 不变。五个限定模块共 64 项通过，Werror 构建、日志、JSON 和源码 hash 在同名 results 目录。

发布载体没有安装权威 graph owner/group route，根引用由实验调用者持有，不能直接替代生产属性目录。旧 descriptor、变更列与空列尚未执行持久 retirement，也没有 failpoint/crash 重开验证。规划缓冲与旧组校验尚未统一预算/取消；没有异构类型回退。后续必须补齐路由、可达性、回收、故障与实际成本验收，不能据本测试认定 A3 已完成。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
