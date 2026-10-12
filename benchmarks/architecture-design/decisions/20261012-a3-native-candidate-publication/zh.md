# A3 候选封装原生原子发布实验

日期：2026-10-12。

新增 NativePropertyLayoutCandidateWriter，校验一个 metadata/native/property domain、准确下一代身份及源快照；使用现有 LargeValue 计划、内容索引计划和 mutateNativeGraphPagesWithLargeValues，在显式 source generation fence 下发布一个候选容器。没有另建 WAL 或发布协议。

新增原生发布测试，验证 generation 只前进一代、仅分配一个 LargeValue id、读取目标值、旧 fence 重试拒绝且 generation 不变、错误下一代身份拒绝。五个限定模块共 102 项通过，证据在同名 results 目录。

整组 LargeValue 仅为身份/恢复实验载体，未安装 graph route，未做旧数据可达性回收，不能用整组重写替代生产局部更新。writer 尚无统一峰值预算和完整取消，失败恢复测试下一步补齐。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
