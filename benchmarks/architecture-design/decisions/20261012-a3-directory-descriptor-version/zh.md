# A3 目录版本与查询快照分离

日期：2026-10-12。

此前候选目录将自身版本与读取快照 generation 精确相等，若永久用于持久目录，会导致无关提交也要求重写全部目录。新增 decodeFromDescriptor/fieldsFromDescriptor：从真实 snapshot 得到 database_id 和可见 generation，同时要求调用方提供该 snapshot 的权威 group descriptor 所选 directory generation。所选版本非零、不来自未来，必须与目录编码内身份精确匹配。不是简单放宽为任何旧 generation 都接受。

当前 buildSnapshotBound 仍用构建 snapshot generation 作为该目录版本；实验编码未改字节布局，既有精确 snapshot 接口保留严格拒绝。未来持久格式必须明确 offset 16 是目录出生/更新版本，与查询可见 generation 分离，不以每次查询 snapshot 重新填入目录。正式生产格式尚未确定，本实验字节不可被生产 reader 静默采用。

新增真实 metadata 测试创建属性目录后执行无关节点创建，核对原属性保持不变。使用原目录版本可从新 snapshot 重建/读取；伪装为新版本、来自未来及零版本拒绝，旧精确接口仍拒绝新 snapshot。严格构建与四个相关模块 21 项通过，证据位于 results/20261012-a3-directory-descriptor-version，含日志/JSON 与源码 hash。

权威 group descriptor 尚未持久实现，测试用显式已知版本模拟该选择；不能证明恶意传错旧 descriptor 时仍会发现全部过期数据。下一步必须把期望版本绑定到真实快照中的目录根与页引用，否则此接口不准入生产。目录/列/slots 在同一 publication 更新与回收仍待完成。

未运行 benchmark、全量测试，未提交或推送，未更新正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
