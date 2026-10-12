# A3 实际字段枚举与列 presence 对照

日期：2026-10-12。

候选字段目录新增 validateBlockMembership，接受已经冻结/认证的 TypedPropertyBlock::VerifiedImage。验证数据库快照及组身份，解析完整 owner/descriptor 元数据而不再解码值，先验证 owner 集，再将排序 row 对应回真实 owner slot，逐列比对 presence 与目录实际 token。存在 NULL 的字段保留在枚举中；缺失字段不入目录。空组也绑定 block/type，不能接受另一个空组。

校验扫描完整组元数据，最坏工作为 rows×columns（旧原型上限 64×4096），不应在每次单字段点读时强制调用。适用于原型构造/加载/完整性检查候选边界，未来查询信任协议仍需明确原子发布及故障检测责任。这里的 verified image 不含真实持久页 owner/generation 协议，不能据此准入生产。

新增两项测试覆盖 slot/row 顺序不同、NULL presence、有效编码但字段目录不一致、跨快照和空组身份。相关字段枚举、真实快照及旧编码模块共 9 项通过，严格构建成功。证据位于 results/20261012-a3-membership-presence，含日志、JSON 和源码 hash。

持久化、原子发布、删除/分裂/回收及实际空间成本仍待完成。未运行 benchmark、全量测试，未提交或推送，未修改正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
