# A3 原生发布中断回归

日期：2026-10-12。

扩展稀疏组原子写入测试，使用现有 bootstrap failpoint。BEFORE_INACTIVE_WRITE 与 AFTER_PARTIAL_INACTIVE_WRITE 分别中断：内存 metadata 与从文件恢复的 superblock 都等于原始完整 superblock，随后同源快照、同 LSN 正常写入成功。保留原有列复用、新旧快照及 stale fence 用例。

新增 AFTER_INACTIVE_WRITE 用例：写入调用抛出 BootstrapPublishInterrupted，但文件恢复已选择新 generation。销毁 LargeValue/property store 及 snapshot，重开 metadata/native stores；从新快照原生目录找到计划的 descriptor ID，读回完整 descriptor、子列和预期值。该结果证明不能把所有抛异常都解释为未提交，也不应在完整 inactive write 后盲目重复提交。

五个限定模块共 65 项通过。构建启用 Werror，日志、JSON 与源码 hash 在同名 results 目录。failpoint 为确定性注入，重开 metadata/native stores 未关闭宿主文件句柄；不是操作系统进程崩溃、所有 torn-write 位置或磁盘硬件故障证明。

当前 carrier 仍无权威 graph route、旧引用 retirement、统一预算和完整恢复协议；已发布根通过实验保存的逻辑 ID 定位，不能替代生产 route 的恢复。后续继续路由、空列/旧列回收、实际页成本及设计验收。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
