# A3 真实 MetadataSnapshotPin 身份绑定

日期：2026-10-12。

字段枚举候选新增 buildSnapshotBound/fieldsSnapshotBound，直接从调用中的 MetadataSnapshotPin.superblock 取得 database_id 和 generation，避免该通路手工拼接身份。原数字身份入口保留为候选纯单测入口，未宣称禁止所有调用方伪造输入。

新增真实 NativeGraphWritePlanner/PropertyCollectionRootStore 测试：创建具有 token 17 的节点，钉住旧 snapshot，再将实际属性改成 token 18。目录分别由该 snapshot 的真实 PageBackedGraphReadAdapter 读取结果构造；同 owner route 下各自版本读取成功，跨 generation 拒绝，旧存储 snapshot 仍保留旧字段。fixture 清理临时文件。

严格构建与 1 项真实快照、4 项目录原型及 2 项旧编码测试共 7 项通过。证据位于 results/20261012-a3-real-snapshot-binding，含日志、JSON 与源码 hash。该入口没有保存 snapshot pin，也不会自动证明传入 roots 来自相同 snapshot；真实持久字段目录、页认证及原子发布仍须后续实现。身份检查不替代页面 owner/generation/checksum 校验。

未运行 benchmark、全量测试，未提交/推送，未更新正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
