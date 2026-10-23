from pathlib import Path
r=Path('/workspace/zyx/docs/apps/docs/content/docs')
for lang,old,new in [
('zh','剩余验证真实 Catalog／索引／约束源，并在同一发布中安装','真实 Catalog／标量索引／NOT NULL 源的协调准备已有证据，剩余其他既有 schema 组合覆盖及在同一发布中安装'),
('en','remaining work must verify real Catalog/index/constraint sources and install','real Catalog/scalar-index/NOT NULL source preparation has evidence; remaining work must cover the other existing schema combinations and install')]:
 p=r/lang/'zyx/architecture/optimization.mdx';s=p.read_text();assert old in s;s=s.replace(old,new,1);p.write_text(s)
notes={
'zh':'''Native 派生旧值读取现复用字段准备的同一认证 ReadContext；实际 ROW owner 才回到原 ROW 入口，不再在候选字段准备与索引／约束之间切换数据源。证据：`20261023-core-cost-native-cold-derived-source/`。新增用实际 Catalog、两棵实际标量索引树、实际节点标签和 NOT NULL 定义构成的私有候选：热更新必须读取未修改的冷字段以满足约束，并生成原标量索引 COW；删除该冷字段则在发布前拒绝。原根、旧索引键、文件、I/O epoch 不变，账户退款，公开模式 guard 保留。仅恢复修复前旧值读取的同编译参数单对象对照，会把未修改的冷字段当成缺失而误报 `required_cold` 非空违例；当前源码通过相同测试。对照源码／编译／链接／失败报告保留，不能计入当前通过项。

30 项定向检查通过，实验两种 membership 各 508 项相关模块、默认 154 项通过，无失败／跳过。测试构造必须对新索引使用既有 stageScalarIndexCreate，不能对不存在的树使用 upsert；初始失败报告保留。这证明实际 schema／索引源与候选字段的准备协调，不是已公开转换／物理安装，也不替代其余 schema 组合、全局降级／owner／逆转换、完整空间及最终性能验收。进度仍为 90%，无全量单测／性能矩阵／提交／推送。''',
'en':'''Native derived old-value reads now reuse field preparation’s same authenticated ReadContext; only actual ROW owners fall back to the original ROW entry. Field preparation and index/constraint planning no longer switch sources. Evidence: `20261023-core-cost-native-cold-derived-source/`. A new private candidate uses actual Catalog records, two actual scalar index trees, an actual Node label and a NOT NULL definition. A hot update must read an unchanged cold field to satisfy the constraint and prepare original scalar-index COW; removing that field refuses before publication. Original roots/index keys/files/I/O epoch stay unchanged, accounts refund and public guards remain. A single-object control restoring only the previous old-value route under identical compilation flags falsely reports the unchanged cold field missing (`required_cold` NOT NULL violation); current source passes the same test. Control source/compile/link/failure reports are retained and are not passing-current evidence.

30 targeted cases pass, both experimental membership variants pass508 related module cases, default154, no failures/skips. New index fixtures must use original stageScalarIndexCreate rather than upsert against absent trees; original setup failures remain. This proves real schema/index source coordination during candidate preparation, not public conversion/physical installation. Other schema combinations, global demotion/owner/inverse, whole-space and final performance gates remain open. Progress90%, no full suite/performance matrix/commit/push.'''}
for lang,note in notes.items():
 p=r/lang/'zyx/architecture/native-single-file-storage-plan.mdx';p.write_text(p.read_text()+'\n\n'+note+'\n')
