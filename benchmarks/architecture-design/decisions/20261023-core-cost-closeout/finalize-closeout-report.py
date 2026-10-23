from pathlib import Path
import csv,json,shutil
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-closeout';rows=list(csv.DictReader((d/'comparison.csv').open()));primary={'label_scan_filter','aggregation_count_by_group','multi_key_group','property_set','edge_batch_1000','fresh_import'}
guards=[]
for row in rows:
 if row['case'] in primary:continue
 a=float(row['final_zyx_p50_ms']);b=float(row['baseline_zyx_p50_ms']);x=float(row['final_zyx_p95_ms']);y=float(row['baseline_zyx_p95_ms'])
 guards.append({'scale':row['scale'],'case':row['case'],'p50_ratio':a/b,'p95_ratio':x/y,'within_5_percent':a<=1.05*b and x<=1.05*y})
(d/'guard-gates.json').write_text(json.dumps(guards,indent=2)+'\n')
records=[json.loads(p.read_text()) for p in sorted((r/'benchmarks/architecture-design/results/20261022-core-cost/closeout/workers').glob('*.json'))]
with (d/'import-throughput.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['scale','engine','family','round','loaded_rows','timed_ms','rows_per_second','worker_wall_seconds','event_file_bytes'])
 for rec in records:
  for event in rec['samples']:
   if event['case']=='fresh_import':w.writerow([rec['scale'],rec['engine'],rec['family'],rec['round'],event['loaded_rows'],event['latency_ms'],event['loaded_rows']*1000/event['latency_ms'],rec['wall_seconds'],event.get('file_bytes')])
labels={'label_scan_filter':'标签过滤','aggregation_count_by_group':'真实单键分组','multi_key_group':'多键分组','property_set':'单属性 SET','edge_batch_1000':'1000 边批写','fresh_import':'导入（等价事务对照）'}
lines=['# 本轮收尾记录：实现冻结，P7 未通过','','本轮已停止扩展实现并完成最终统一测量；**验收仍为 90%，不是全部优化完成或 Kuzu 目标达成。** 前向冷转换／完整目标构建与全库空间准入仍未交付，公开入口保持关闭，不以私有夹具或 8 组前缀绕过门禁。当前结果基于真实默认格式生产路径，不使用未采用的实验布局制造收益。','','## 已完成的固定清单工作','','- 跨组／混合 Node、Edge 的冷热修改共用一套私有 FieldPage，合并 namespace 目录后一次原子发布；包含混合创建、删除、整列降级、逻辑 API、旧 pin／integrity，以及全批超过 64 实际变化或迁移 owner 的写前拒绝。','- 修复 4096 字节缓存的完整查询：单帧顺序扫描使用原有有界复制路径，不把唯一帧的 pin 留给后续消费者或换页；不增加缓存额度或减少校验。保存修改前的失败报告与异常栈。','- 必要模块验证：实验两种 membership 配置各 564 项、默认 154 项、缓存两种额度的完整查询 2 项均通过；每个实验报告包含 27 项持久化生命周期用例。没有执行全量单测。','','## 最终性能','','99／99 独立进程全部通过语义 oracle。每类查询／写入 3 个独立进程、每进程 3 次预热＋10 次正式测量；每数据集导入 3 次。下表为正式计时 P50（毫秒），不含排除在合同外的准备／核验；完整进程墙钟另列在资源表。','','| 数据集 | 场景 | 优化前 | 当前 ZYX | 当前 Kuzu 等价对照 | 提速 | ZYX／Kuzu |','|---|---|---:|---:|---:|---:|---:|']
for scale in ['medium','holdout']:
 for case in ['label_scan_filter','aggregation_count_by_group','multi_key_group','property_set','edge_batch_1000','fresh_import']:
  row=next(v for v in rows if v['scale']==scale and v['case']==case)
  lines.append('| '+' | '.join([scale,labels[case],*[f'{float(row[k]):.3f}' for k in ['baseline_zyx_p50_ms','final_zyx_p50_ms','current_kuzu_equivalent_p50_ms','speedup_vs_baseline','zyx_over_kuzu']]])+' |')
lines+=['','SET 约提升 12.9–14.0 倍，1000 边批写约提升 12.2–12.4 倍，导入约提升 6.3–6.7 倍。过滤／分组仅约提升 1.2 倍，仍有数量级差距；六个核心场景在两个数据集上均未达到等价 Kuzu P50。两个数据集的过滤／单键分组没有达到原定 5 倍结构性门槛。所有既定回归守护的 P50/P95 均在 5% 退化线以内。','', 'Kuzu 生产 COPY P50 为 medium 887.417 ms、holdout 784.591 ms；ZYX 分别慢约 46.4／42.9 倍。这与等价 1000 行提交边界对照不同，单独展示，不以较好的一项代替另一项。三个导入样本的最大值不是可靠 P95。','','## 完整证据与复现边界','','- [全部场景及 COPY 对照](comparison.md)、[包含 P95 和样本数的 CSV](comparison.csv)、[逐进程分位数](per-process.csv)、[CPU／RSS／OS I/O／观测文件峰值](resources.csv)、[导入吞吐与正式计时／墙钟](import-throughput.csv)。父进程轮询可能遗漏短时文件峰值，不等于精确空间 census。','- [模块结果与库身份](module-checks.json)、[原始 worker 输出／stderr／逐样本 receipt](worker-evidence.tar.gz)、[最终判定](closeout.json)、[回归门禁](guard-gates.json)、[环境](environment.json)、[冻结](freeze.json)。','- source.patch 加 additional-source.tar.gz 记录相对 freeze.json 中 base_head 的当前代码；脚本、命令、编译选项和依赖保留。大数据库和二进制不进入证据包；input-identities.json 记录实际输入。输入生成与 P0 测量合同在相邻 20261022-core-cost-p0，未更换数据或 oracle。','- 实际平台为 Linux；不冒充 macOS／Windows 验收。large／skew 无等价 Kuzu 项记 N/A；历史 CPU 百分比不冒充本次 profile。两个继承的 metadata 延迟回收失败继续单列。','- 默认5/31、实验13/39，格式拒绝／原子发布／源校验／全库至少10%节省门禁和预算保持原合同；冷源夹具不构成公开转换证明。','- P7 未通过，百分比没有增长，因此按用户规则不提交、不推送。没有以性能落后继续开启新优化分支；未完成目标明确保留，不宣称100%。','']
(d/'README.md').write_text('\n'.join(lines))
for lang in ['zh','en']:
 p=r/f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('最终候选已冻结，统一测量与收尾中','99／99 进程测量完成；目标未达成，本轮收尾')
  s+='''\n### 9.3 最终实测结果（2026-10-23）

统一矩阵 99／99 个进程全部通过语义 oracle，完整表与原始数据见 `benchmarks/architecture-design/decisions/20261023-core-cost-closeout/README.md`。medium／holdout 的 SET P50 为 4.845／4.660 ms，较基线提升 14.0／12.9 倍；1000 边批写为 32.649／30.644 ms，提升 12.2／12.4 倍；导入为 41.195／33.692 秒，提升 6.7／6.3 倍。过滤仅提升约 1.2 倍，仍慢于 Kuzu 71.3／63.8 倍；真实单键分组仍慢约 41.2／39.7 倍。Kuzu COPY 对照单列，不能与等价事务导入混为一谈。所有既定回归守护的 P50/P95 在 5% 退化线以内。

P7 及 Kuzu 目标明确未达成，验收仍 90%；前向冷转换／全目标空间准入没有交付。按本轮收尾边界停止扩展，保留未达成条件；百分比未增长，不提交、不推送。\n'''
 else:
  s=s.replace('Final candidate frozen; unified measurement and closeout underway','99/99 workers measured; objective unmet, round closed')
  s+='''\n### 9.3 Final measurements (2026-10-23)

All99 workers pass their semantic oracles. Complete tables and raw evidence: `benchmarks/architecture-design/decisions/20261023-core-cost-closeout/README.md`. Medium/holdout SET P50 is4.845/4.660ms, improving14.0/12.9x;1000-edge writes are32.649/30.644ms, improving12.2/12.4x; import is41.195/33.692seconds, improving6.7/6.3x. Filtering improves only about1.2x and remains71.3/63.8x slower than Kuzu; actual single-key grouping remains41.2/39.7x slower. Show COPY separately from equivalent-transaction imports. All registered guards remain within the5% P50/P95 regression limit.

P7 and Kuzu objectives are explicitly unmet; acceptance remains90%. Forward cold conversion/whole-target admission remains undelivered. Stop expanding at this round's closeout boundary, preserve unmet conditions, and do not commit/push because progress did not increase.\n'''
 p.write_text(s)
shutil.copy2('/tmp/finalize-closeout-report.py',d/'finalize-closeout-report.py')
print('final closeout report written; acceptance90%, objective unmet')
