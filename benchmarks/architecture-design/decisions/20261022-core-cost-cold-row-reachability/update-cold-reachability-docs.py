from pathlib import Path
root=Path('docs/apps/docs/content/docs')
for lang in ['zh','en']:
 base=root/lang/'zyx/architecture'
 p=base/'optimization.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('隔离夹具不是公开转换后的真实查询或全库验收。接下来', '既有回收扫描器现已合并普通 ROW、显式 owner 与冷整根引用；无引用尾部也以有界逻辑探针推进，证据见 `20261022-core-cost-cold-row-reachability/`。隔离夹具不是公开转换后的真实查询或全库验收。接下来',1)
  s=s.replace('公开转换后的全库完整性、LargeValue 可达性与退役', '公开转换后的全库完整性及 LargeValue 回收／旧 pin／退役验收',1)
 else:
  s=s.replace('Isolated fixtures are not real-query or whole-database acceptance after public conversion. Next', 'The existing reclamation scanner now merges ordinary ROW, explicit owners and cold whole-root references; reference-free tails advance through bounded logical probes. Evidence is in `20261022-core-cost-cold-row-reachability/`. Isolated fixtures are not real-query or whole-database acceptance after public conversion. Next',1)
  s=s.replace('whole-database integrity after public conversion, LargeValue reachability and retirement', 'whole-database integrity and LargeValue reclamation/old-pin/retirement acceptance after public conversion',1)
 p.write_text(s)
 p=base/'native-single-file-storage-plan.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('**生产发布仍关闭。** 正反字段图核验不能补足旧 LargeValue ownership iterator 对冷 owner 的遗漏。下一步必须按稳定逻辑 owner／group 边界补齐有界可达性枚举、实际引用保留与退役，再完成转换和变更生命周期；不得从不完整的 mark 或未耗尽的扫描推导可回收性。','**生产发布仍关闭。** 既有回收扫描器已接入冷整根枚举，正反字段图核验仍不能替代实际公开转换后的回收／旧 pin／退役及完整变更生命周期；不得从不完整的 mark 或未耗尽的扫描推导可回收性。')
  s=s.replace('可达性扫描的下一实施合同：', '可达性扫描实现合同（已接入既有扫描器）：')
  s+='\n\n冷引用扫描证据见 `20261022-core-cost-cold-row-reachability/`。新测试使用原私有候选描述符和实际页／索引／Entity Map，通过私有测试入口执行生产扫描循环，不另写模拟遍历；生产入口仍从实际持久描述符打开。冷整根只交付一次引用，由既有 sink 认证实际 manifest，不读取完整值内容。共用同步冷块窗口不增加缓存或格式位。组探测／冷块访问显式计入保守目录工作预算，避免单叶游标没有 sibling boundary key 时遗漏计费；预算上限不变。旧混合夹具的引用仍为 5 个，新增无引用组尾探针计一个工作单位，因而总工作单位由 5 变 6，恢复后的尾部由 3 变 4；引用顺序／集合和最终 cursor 不变。初始预算测试失败及修复记录保留。此证据只完成扫描组件，不授予未完成的公开转换／实际回收验收，进度仍 90%。\n'
 else:
  old='**Production publication remains closed.**'
  # Replace the exact obsolete paragraph only, preserving other publication guards.
  paragraphs=s.split('\n\n')
  for i,v in enumerate(paragraphs):
   if v.startswith(old) and ('iterator' in v or 'reachability' in v):
    paragraphs[i]='**Production publication remains closed.** The existing reclamation scanner now enumerates cold whole roots. Read-only field ownership still cannot replace reclamation/old-pin/retirement acceptance after actual public conversion or the complete mutation lifecycle. Partial marks and non-exhausted scans never authorize reclamation.'
  s='\n\n'.join(paragraphs)
  s=s.replace('Next reachability implementation contract:', 'Reachability implementation contract (now integrated into the existing scanner):')
  s+='\n\nCold-reference scanner evidence is in `20261022-core-cost-cold-row-reachability/`. New tests use the original private candidate descriptor and actual pages/indexes/Entity Map, entering the production scan loop through a private test seam rather than a mirrored traversal. Production still opens the actual persisted descriptor. Each cold whole root delivers one reference to the existing manifest-authenticating sink without reading complete value contents. The shared synchronous cold window adds no cache or format bit. Group probes/cold-block visits explicitly charge conservative directory work, including single-leaf cursors with no sibling boundary key; budget limits are unchanged. The old mixed fixture still contains five references; a reference-free group-tail probe adds one work unit, changing total work from five to six and resumed-tail work from three to four. Reference order/set and final cursor are unchanged. Initial budget-test failure and correction are retained. This completes the scanner component, not outstanding public-conversion/actual-reclamation acceptance; progress stays 90%.\n'
 p.write_text(s)
 p=base/'native-capability-acceptance-matrix.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('冷 ROW 读取及字段图的只读正反 ownership 已接入', '冷 ROW 读取、字段图的只读正反 ownership 及既有回收扫描器中的冷整根枚举已接入')
 else:
  s=s.replace('Cold-ROW reading and read-only forward/reverse field ownership are integrated', 'Cold-ROW reading, read-only forward/reverse field ownership and cold whole-root enumeration in the existing reclamation scanner are integrated')
 p.write_text(s)
