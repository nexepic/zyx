"""Reconcile original and targeted correction evidence without hiding failures."""
from pathlib import Path
import csv,hashlib,json,math,statistics
HERE=Path(__file__).resolve().parent
BASE=Path('/workspace/zyx/benchmarks/architecture-design/results/20261024-work-scan')
OLD=BASE/'final-delivery';NEW=BASE/'s4-correction-window'
def sha(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def key(r):return (r['scale'],r['family'],r['engine'],r['round'])
original=[json.loads(p.read_text()) for p in sorted((OLD/'workers').glob('*.json'))]
assert len(original)==171
correction=json.loads((NEW/'evaluation.json').read_text());assert correction['passed']==33 and correction['samples']==510
replacements={key(r):r for r in correction['results']}
merged=[];provenance=[]
for old in original:
 k=key(old)
 newkey=(k[0],'repaired-skew',k[2],k[3]) if k[0]=='skew' else k
 if newkey in replacements:
  r=replacements.pop(newkey);merged.append(r);provenance.append({'key':k,'original_status':old['status'],'original_freeze':old['freeze_sha256'],'replacement_freeze':r['freeze_sha256'],'reason':'Restore independently verified registered graph' if k[0]=='skew' else 'Native ROW placement and source-pool COW corrections'})
 else:merged.append(old)
assert not replacements and len(merged)==171 and all(r['status']=='passed' for r in merged)
assert sum(len(r['samples']) for r in merged)==3660
values={}
for r in merged:
 for e in r['samples']:values.setdefault((r['scale'],e['case'],r['engine']),[]).append(e['latency_ms'])
stats={k:{'samples':len(v),'p50_ms':statistics.median(v),'p95_ms':sorted(v)[math.ceil(.95*len(v))-1]} for k,v in values.items()}
with (HERE/'summary.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['scale','case','engine','samples','p50_ms','p95_ms'])
 for k,v in sorted(stats.items()):w.writerow([*k,*v.values()])
with (HERE/'process-costs.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['scale','family','engine','round','freeze_sha256','process_wall_seconds','process_user_seconds','process_system_seconds','process_peak_rss_bytes','os_read_bytes','os_write_bytes','observed_private_file_peak_bytes','sum_measured_query_ms'])
 for r in merged:
  cost=r['resources'];w.writerow([r['scale'],r['family'],r['engine'],r['round'],r['freeze_sha256'],r['wall_seconds'],cost['user_seconds'],cost['system_seconds'],cost['peak_rss_bytes'],cost['os_read_bytes'],cost['os_write_bytes'],r['observed_private_file_peak_bytes'],sum(e['latency_ms'] for e in r['samples'])])
old_eval=json.loads((OLD/'evaluation.json').read_text());primary=old_eval['primary'];guards=[]
for g in old_eval['guards']:
 g=dict(g);scale,case,engine=g['scale'],g['case'],g['engine'];candidate=stats[(scale,case,engine)]
 changed = engine=='B' and case in ['edge_batch_1000','edge_create_delete']
 bases = ['control'] if changed else ['initial','control']
 ratios={base:{metric:candidate[metric]/stats[(scale,case,base)][metric] for metric in ['p50_ms','p95_ms']} for base in bases}
 guards.append({'scale':scale,'case':case,'engine':engine,'ratios':ratios,'exceeds_five_percent_review_threshold':any(v>1.05 for r in ratios.values() for v in r.values()),'evidence':'targeted correction with contemporaneous control; historical initial timings are not a same-window comparator' if changed else ('repaired registered-graph correction' if scale=='skew' else 'original final window')})
report={'roadmap_progress_percent':100,'performance_superiority_claimed':False,'beats_kuzu':False,'fixed_5x_required':False,'kuzu_superiority_required_for_roadmap_delivery':False,'original_window':{'processes':171,'passed':162,'failed':9,'validated_samples':3390,'freeze_sha256':sha(OLD/'freeze.json')},'targeted_correction':{'processes':33,'passed':33,'samples':510,'freeze_sha256':sha(NEW/'freeze.json')},'executed_processes_total':204,'validated_sample_records_retained_total':3900,'retained_failed_processes':9,'reconciled_evidence':{'processes':171,'validated_samples':3660,'all_complete_oracles_passed':True,'versions':'Explicit composite: original measured read and unchanged guard binaries plus targeted final write binaries and versioned Skew fixture. This is not a new single-binary full campaign.'},'replacement_provenance':provenance,'primary':primary,'guards':guards,'all_performance_guards_passed':not any(g['exceeds_five_percent_review_threshold'] for g in guards),'default_format':'5/31 ROW; unchanged automatic adoption policy','experimental_format':'15/41 explicit offline conversion; evaluate scan reuse and point/write costs before opting in','space':{'medium':{'row_bytes':309985280,'scan_bytes':256507904,'saving_percent':100*(1-256507904/309985280)},'holdout':{'row_bytes':259915776,'scan_bytes':222035968,'saving_percent':100*(1-222035968/259915776)}},'generality_file_cost':{'source_bytes':(BASE/'generality.scan-ready.zyx').stat().st_size,'after_512_mutation_batches_bytes':128712704,'long_lived_old_pin':True,'initial_conversion_savings_are_not_a_post_update_growth_guarantee':True},'validation_platforms_executed':['Linux x86_64 clang 19'],'platforms_not_executed':['macOS','Windows'],'full_unit_suite_run':False}
(HERE/'report.json').write_text(json.dumps(report,indent=2)+'\n')
lines=['# Roadmap delivery and measured limits','','The implementation roadmap is complete under the user\'s revised completion criterion. Kuzu remains faster in all eight primary read comparisons. 33 of 42 guard combinations exceed the 5% review threshold in at least one statistic; this is a review flag rather than proof that each excess is a stable regression; this report does not label those guards as passes. The new format remains explicit and experimental, with the default ROW path unchanged.','','## Read comparison (original final window)','','All figures are P50 milliseconds from the same original window; write-only corrections were not represented as new read timings.','','| Dataset | Case | Initial ZYX | Control ZYX | Selected scan path | Kuzu |','|---|---|---:|---:|---:|---:|']
for p in primary:
 scale,case=p['scale'],p['case'];lines.append('| '+scale+' | '+case+' | '+' | '.join(f"{stats[(scale,case,e)]['p50_ms']:.3f}" for e in ['initial','control','B','kuzu'])+' |')
lines+=['', '## Corrected write comparison', '', '| Dataset | Case | Scan path P50 / P95 ms | Same-window control P50 / P95 ms |', '|---|---|---:|---:|']
for scale in ['medium','holdout']:
 for case in ['edge_batch_1000','edge_create_delete']:
  a=stats[(scale,case,'B')];b=stats[(scale,case,'control')];lines.append(f"| {scale} | {case} | {a['p50_ms']:.3f} / {a['p95_ms']:.3f} | {b['p50_ms']:.3f} / {b['p95_ms']:.3f} |")
lines+=['','## Validation and scope','','The original 171-process window retained nine failed Skew workers. Independent graph inspection proved that their input contained 32 nodes and no edges. A fresh, versioned copy restores the preregistered 512 edges; the oracle is unchanged. The 33-process correction window replaces those nine workers and 24 affected write workers. The two windows execute 204 processes in total, retaining nine original failures and 3900 validated sample records including superseded write samples. Reconciled evidence selects 171 successful process records and 3660 validated samples, with explicit per-record provenance and the original failures retained.','','The final three engine edits affect write placement/publication only. The original read timings identify their archived binaries and c8b37bc3 source; the correction identifies its own production binaries, source freeze and input hashes. No entire performance matrix or full unit suite was repeated.','','Corrected write comparisons use only the contemporaneous frozen control. Historical initial timings remain visible but are not treated as same-window write comparators. `process-costs.csv` reports process wall/CPU, RSS, OS byte counters, observed file peaks and measured-query sums separately; their difference is not an attribution of startup or I/O time.', '', 'See `report.json` and `summary.csv` for P50/P95, guard ratios, frozen identities, and adoption limits. See the sibling S4 validation directory for module results, complete public oracles, old-pin and 4 KiB-cache generality checks, and before/after regression tests.','','## Adoption costs and future work','','Steady whole-file savings are 17.25% on medium and 14.57% on holdout, including the admitted images\' retained structures. Original conversion costs remain about 207 and 136 minutes respectively; conversion is not free. Historical file polling is an observed lower-bound peak, and process RSS high-water marks are not exact allocator peaks. No DRAM-traffic or hardware-stall claims are inferred from sizeof counters or nested timers.','','Initial conversion savings are not a promise after updates. The preregistered 512-batch generality case with a long-lived old pin grows from '+str((BASE/'generality.scan-ready.zyx').stat().st_size)+' to 128712704 bytes; snapshot retention and mutation amplification are material adoption costs.', '', 'Known point/property overheads and the substantial Kuzu gap remain future optimization work. Use the scan format for a measured scan-heavy workload that amortizes conversion and accepts the documented point/write costs; retain ordinary ROW for workloads that do not. Large/Skew native conversion refusals remain explicit N/A. Linux was executed; macOS and Windows were not executed in this environment.']
(HERE/'README.md').write_text('\n'.join(lines)+'\n')
zh=['# 优化路线交付与性能边界', '', '按用户最新要求，本轮优化路线完成至 100%。Kuzu 仍在八项主查询中全部更快；42 组守护中 33 组至少一项超过 5% 复核阈值，不等同于每项均已证实为稳定退化，未宣称全部性能守护通过。默认 ROW 格式不自动切换，15/41 保持显式实验及离线转换采用。', '', '## 主查询对照', '', '下表为原最终窗口 P50 毫秒，绑定原已归档二进制及 c8b37bc3 读取实现。写入修正未被描述为新二进制的读取实测。', '', '| 数据集 | 场景 | 初始 ZYX | 控制 ZYX | 选定扫描路径 | Kuzu |', '|---|---|---:|---:|---:|---:|']
for p in primary:
 scale,case=p['scale'],p['case'];zh.append('| '+scale+' | '+case+' | '+' | '.join(f"{stats[(scale,case,e)]['p50_ms']:.3f}" for e in ['initial','control','B','kuzu'])+' |')
zh+=['', '## 写入修正与验证来源', '', '| 数据集 | 场景 | 修正后扫描格式 P50/P95 ms | 同窗口控制 P50/P95 ms |', '|---|---|---:|---:|']
for scale in ['medium','holdout']:
 for case in ['edge_batch_1000','edge_create_delete']:
  a=stats[(scale,case,'B')];b=stats[(scale,case,'control')];zh.append(f"| {scale} | {case} | {a['p50_ms']:.3f} / {a['p95_ms']:.3f} | {b['p50_ms']:.3f} / {b['p95_ms']:.3f} |")
zh+=['', '原最终窗口 171 个进程中，162 个通过，9 个 Skew 输入缺边失败；3390 个样本通过完整 oracle。独立检查确认原 Skew 文件仅有 32 个节点、没有边。新副本恢复预登记 512 条边，oracle 不变。定向窗口仅包含两个修正写入场景和修复后的 Skew，共 33 个进程、510 个样本。两窗口累计执行 204 个进程，保留原 9 个失败及含被替代写入样本在内的 3900 个验证通过的样本记录。按来源替换 24 个写入及 9 个 Skew 记录后，最终组合选取 171 个有效进程记录、3660 个完整结果通过的样本。原始失败及逐记录替换来源全部保留。这是显式版本组合，未重跑全矩阵，也不是一次新二进制全场景测量。', '', '最终三个引擎源码修正仅涉及写入。修正写入只与新窗口内交替测量的冻结控制版本比较；旧初始写入延迟保留，但不作为同窗口对照。report.json 和 summary.csv 包含各场景 P50/P95、守护比值、来源及采用限制。process-costs.csv 单列进程墙钟/用户及系统 CPU、RSS 高水位、操作系统字节计数、文件轮询峰值及正式操作耗时和；两类耗时之差不是精确启动或 I/O 归因。', '', '最终三配置模块测试 113/25/89 项通过，两库 10 组公开完整 oracle 通过，预登记 16384 节点/512 批混合修改、旧快照、完整性及 4 KiB 缓存重开通过；未运行全量单测。实际执行 Linux，未执行 macOS/Windows。原生 Large/Skew 转换拒绝保持明确 N/A，未宣称原生采用成功。', '', '## 成本与后续范围', '', '初次转换的空间收益不代表更新后的增长保证。预登记泛化输入在长期旧 pin 和 512 批修改后，从 '+str((BASE/'generality.scan-ready.zyx').stat().st_size)+' 增至 128712704 字节；版本保留及修改放大是实际采用代价。', '', 'medium/holdout 实际整库空间节省 17.25%/14.57%，转换约耗时 207/136 分钟，不以转换免费为前提。文件轮询只提供已观察峰值的下界，RSS 高水位不是精确分配峰值，不由 sizeof 或嵌套 timer 推断 DRAM 流量。扫描密集且能摊销转换、能接受点查/写入代价的工作负载才适合显式采用；其余保留 ROW。Kuzu 差距和剩余点查/属性开销留待后续优化，本轮完成后不自动展开无关项目。']
(HERE/'README.zh.md').write_text('\n'.join(zh)+'\n')
print('Reconciled 171 passing records / 3660 samples; Kuzu and guard limits reported honestly')
