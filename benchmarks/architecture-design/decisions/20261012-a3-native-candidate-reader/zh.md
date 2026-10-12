# A3 候选封装原生读取与重开

日期：2026-10-12。

新增实验 NativePropertyLayoutCandidateReader，复用 LargeValueStore 的 manifest、快照与内容认证。要求专用 media、BYTES、正数预算、非空准入回调和不晚于查询快照的明确 birth generation；身份由原生 database id 与外部权威路由提供的 birth version 组成。读取后通过完整 codec 解码并生成不可变验证对象，取消检查传入所有现有边界。

新增真实原生存储测试：64 owner 共享字段镜像经现有 LargeValue writer 发布后读取、预算拒绝、发布前旧快照不可读取新引用、重开数据库后首 owner 值保持。五个限定模块共 101 项通过，同名 results 目录保存日志、JSON 与源码 hash。fixture 清理临时数据库。

该引用尚未安装到 authoritative graph route，测试由外部保存引用/槽位表/version；不能宣称生产布局已切换。整个组封装为单个 LargeValue 只作为恢复/身份实验载体，局部更新不得据此重写整组。峰值预算、正式发布协议和可达性回收仍待完成。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
