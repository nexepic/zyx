# A3 字段目录的数据库与快照身份

日期：2026-10-12。

槽位 generation 仅识别删除/复用，无法区别同 owner、同 slot 下字段集发生更新的不同快照。原型 build 和 fields 现都要求显式 SnapshotIdentity（非零 database_id/generation）；目录持有构建时身份，读取必须匹配，再校验 owner route 与目录自己的 slot generation。禁止隐式默认快照或零身份。

新增测试保持 owner route 完全相同，两个版本分别含 token 1/2。各自旧/新目录在匹配身份时可读取，不匹配 generation 或 database 时拒绝，并拒绝构造零身份。四项目录原型测试加两项旧布局编码对照共 6 项通过，严格构建成功。证据位于 results/20261012-a3-membership-snapshot-identity，包含日志/JSON 与源码 hash。

此身份目前是调用方提供的实验合同，尚未绑定真实 MetadataSnapshotPin/页认证或持久 group generation；它不能证明实际存储页与快照一致，也不授权跨快照缓存复用。持久化、原子发布、删除/分裂/回收及实际页成本仍待完成。实验原型不接入生产格式。

未运行 benchmark、全量测试，未提交或推送，未修改正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
