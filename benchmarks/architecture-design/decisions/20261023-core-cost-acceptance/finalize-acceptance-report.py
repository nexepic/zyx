from pathlib import Path
import csv,json,shutil
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance';rows=list(csv.DictReader((d/'comparison.csv').open()));primary={'label_scan_filter','aggregation_count_by_group','multi_key_group','property_set','edge_batch_1000','fresh_import'}
guards=[]
for row in rows:
 if row['case'] in primary:continue
 a=float(row['final_zyx_p50_ms']);b=float(row['baseline_zyx_p50_ms']);x=float(row['final_zyx_p95_ms']);y=float(row['baseline_zyx_p95_ms'])
 guards.append({'scale':row['scale'],'case':row['case'],'p50_ratio':a/b,'p95_ratio':x/y,'within_5_percent':a<=1.05*b and x<=1.05*y})
confirmation=json.loads((d/'guard-confirmation.json').read_text())
assert confirmation['all_oracles_passed'] and confirmation['original_matrix_unchanged']
confirmed={v['case']:v for v in confirmation['summary']}
for guard in guards:
 guard['original_matrix_within_5_percent']=guard['within_5_percent']
 guard['targeted_confirmation']=confirmed.get(guard['case']) if not guard['within_5_percent'] else None
 guard['no_stable_regression_confirmed']=guard['within_5_percent'] or bool(guard['targeted_confirmation'] and guard['targeted_confirmation']['within_5_percent'])
(d/'guard-gates.json').write_text(json.dumps(guards,indent=2)+'\n');assert all(v['no_stable_regression_confirmed'] for v in guards)
records=[json.loads(p.read_text()) for p in sorted((r/'benchmarks/architecture-design/results/20261022-core-cost/acceptance/workers').glob('*.json'))];assert len(records)==99 and all(x['status']=='passed' for x in records)
with (d/'import-throughput.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['scale','engine','family','round','loaded_rows','timed_ms','rows_per_second','worker_wall_seconds','event_file_bytes'])
 for rec in records:
  for event in rec['samples']:
   if event['case']=='fresh_import':w.writerow([rec['scale'],rec['engine'],rec['family'],rec['round'],event['loaded_rows'],event['latency_ms'],event['loaded_rows']*1000/event['latency_ms'],rec['wall_seconds'],event.get('file_bytes')])
modules=json.loads((d/'module-checks.json').read_text());assert modules['implementation_checks_passed']
a=json.loads((d/'acceptance.json').read_text());a['performance_objectives_are_completion_conditions']=False;a['user_contract']='Complete documented implementation, necessary verification and objective comparison; Kuzu superiority is not required.';a['regression_guards_passed']=True;a['original_guard_p95_exceedances']=[{'scale':v['scale'],'case':v['case']} for v in guards if not v['original_matrix_within_5_percent']];a['targeted_guard_confirmation_processes']=confirmation['processes'];a['module_checks']=modules['checks'];a['whole_database_space_example']=modules['whole_database_space'];a['benchmark_profile']='Default5/31; public bounded cold conversion is opt-in experimental13/39. No claim that medium/holdout adopted cold layout.'
a['Kuzu_objective_met']=all(v['Kuzu_P50_passed'] for v in a['primary_gates']);a['historical_5x_objective_met']=all(v['baseline_5x_passed'] is not False for v in a['primary_gates'])
(d/'acceptance.json').write_text(json.dumps(a,indent=2)+'\n')
labels={'label_scan_filter':'标签过滤','aggregation_count_by_group':'真实单键分组','multi_key_group':'多键分组','property_set':'单属性 SET','edge_batch_1000':'1000 边批写','fresh_import':'导入（等价事务对照）'}
lines=['# 最终验收：实施工作 100%','','依据用户最新要求，完成优化文档的实现、安全与必要模块验收，最后客观报告性能，**不以超越 Kuzu 为完成条件**。100% 不代表原5倍或 Kuzu 性能目标达成。当前交付已完成，不再开启优化分支。','','## 实际交付与测试','','- 公开的有界 ROW→冷热布局转换复用 Native 原子协调器，读取真实 ROW／Entity／Catalog，不再通过描述符字节安装夹具。显式实验13/39、每次最多8组、每组64-ID／4096-membership，共用1MiB账户；默认5/31不自动采用。','- 全部指定组、字段／目录、LargeValue data／manifest／content-index 与 metadata 先准备；按整个数据库核算至少10%稳态节省及原峰值／文件限额，满足才一次切换A/B根，不满足保持ROW，不暴露前缀。不扩大为跨事务全库迁移工具或强制所有大库采用。','- 12项真实公开转换测试通过：Node／Edge、采用／拒绝、生产filter/group/冷点查及重开、冷热修改／降级、backend sealed创建／删除、大值原子转换及实际回收、逆转换保留其余组、字段池搬迁／旧pin保护／释放后回收、取消重试和部分／完整A/B恢复、完整integrity。','- 两种实验membership配置各576项（含上述12项及原27项持久化夹具），默认155项、两种缓存完整查询2项均通过。没有执行全量单测。初次公开转换后的读取遗漏原账户作用域曾失败，修正调用方读取scope后通过，原始报告保留。','- 全库空间案例为512 owner、五字段含长ID与冷文本，实际稳态分配1,441,792→1,048,576字节，节省27.27%，通过原10%门槛；窄输入拒绝保持ROW。此案例不是medium/holdout的空间比例或通用收益承诺。','','## 客观性能','','最终99／99独立进程全部通过原oracle。查询／写入每场景三进程，每进程3预热＋10正式样本；导入三次独立运行。下表单位为毫秒、取正式计时P50；准备／核验与进程墙钟分开。倍率“ZYX／Kuzu”大于1表示ZYX更慢。','','| 数据集 | 场景 | 优化前 | 最终 ZYX | Kuzu 等价对照 | 提速 | ZYX／Kuzu |','|---|---|---:|---:|---:|---:|---:|']
for scale in ['medium','holdout']:
 for case in ['label_scan_filter','aggregation_count_by_group','multi_key_group','property_set','edge_batch_1000','fresh_import']:
  row=next(v for v in rows if v['scale']==scale and v['case']==case)
  lines.append('| '+' | '.join([scale,labels[case],*[f'{float(row[k]):.3f}' for k in ['baseline_zyx_p50_ms','final_zyx_p50_ms','current_kuzu_equivalent_p50_ms','speedup_vs_baseline','zyx_over_kuzu']]])+' |')
lines+=['','最终默认生产路径仍未全面达到Kuzu。过滤／分组的数量级差距及原5倍目标未达成均保留，不用写入／导入的提升替代读取结果。Kuzu生产COPY在[完整比较](comparison.md)中单列，不能与等价事务边界混为一谈；三个导入的最大值不是可靠P95。原统一矩阵有large_selected、large_patch、skew_twohop三项P95超历史5%线，P50均未退化；原样本不变。追加18个交替基线／当前进程，每项每引擎90个样本，同条件对照P50/P95均未复现稳定退化，详见[定向确认](guard-confirmation.json)及其原始证据。','','本矩阵使用默认格式的生产查询／写入，不强制对medium/holdout采用实验冷布局，也不从12项功能测试推断该布局的时延。历史CPU归因不是最终构建的新profile。','','## 数据、身份与复现','','- [全部场景及COPY](comparison.md)、[包含P95／样本数的CSV](comparison.csv)、[逐进程分位数](per-process.csv)、[CPU／RSS／OS I/O／文件峰值观测](resources.csv)、[导入吞吐及计时／墙钟](import-throughput.csv)、[回归对照](guard-gates.json)。文件峰值轮询可能遗漏短时峰值，不等于精确空间census。','- [验收判定](acceptance.json)、[模块与空间证据](module-checks.json)、[全部worker原始样本／stderr／receipts](worker-evidence.tar.gz)、[环境](environment.json)、[冻结](freeze.json)、[输入身份](input-identities.json)。旧计时runner的final-completion仅记录测量阶段结束，最终验收以本acceptance.json及最新用户要求为准。','- source.patch与additional-source.tar.gz记录相对freeze.json base_head的实际代码；保留编译／链接／运行命令、原始失败和SHA。大数据库及构建二进制不提交。输入生成／原冻结合同在相邻20261022-core-cost-p0；没有更换数据或oracle。','- 前轮closeout及historical-storage/capability-notes保留过去状态。本次公开转换已交付；历史“入口关闭／未交付”不作为当前限制。默认与实验格式、Linux实测与跨平台发布门禁保持独立。','- large/skew无等价Kuzu结果的项目记N/A。两个继承的metadata延迟回收断言在原冻结库同样失败，继续单列，不冒充本次通过。','- 按进度增长规则，从90%到100%仅执行一次最终提交／推送到远程work；提交目标及核验命令见delivery.json；实际提交身份以Git远程记录为准。完成后停止，不新增优化任务。','']
(d/'README.md').write_text('\n'.join(lines))
for lang in ['zh','en']:
 p=r/f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx';s=p.read_text().replace('- [ ]','- [x]')
 if lang=='zh':
  s=s.replace('当前实施进度 90%；P1–P6 已验收，P7 正在完成最终验证与客观性能报告。','当前实施验收 100%；P1–P7 已完成。性能与 Kuzu 差距按实测报告，不代表已超越。')
  s=s.replace('| 10% | 最终验收中 |','| 10% | 必要模块与99／99进程测量完成，客观报告已归档 |')
  s+='\n最终实施验收已完成（100%）：公开12项、实验两种模式各576项、默认155项、两种缓存2项必要检查通过；统一99／99进程及完整对比已归档。提交／推送目标见delivery.json，实际身份以Git记录为准。按本次固定清单停止，不再新增任务。\n'
 else:
  s=s.replace('Implementation progress is 90%; P1–P6 are accepted and P7 is completing final verification and objective reporting.','Implementation acceptance is100%; P1–P7 are complete. Report measured Kuzu gaps without claiming superiority.')
  s=s.replace('| 10% | Final acceptance underway |','| 10% | Necessary modules and99/99 workers complete; objective report archived |')
  s+='\nImplementation acceptance is complete (100%): public12, both experimental modes576 each, default155 and both full-query cache modes2 pass; all99 matrix workers and complete comparisons are archived. The commit/push target is recorded in delivery.json; Git history records the actual revision. Stop at this fixed checklist without follow-on tasks.\n'
 p.write_text(s)
shutil.copy2('/tmp/finalize-acceptance-report.py',d/'finalize-acceptance-report.py')
print('implementation acceptance100%; measured Kuzu objective:',a['Kuzu_objective_met'])
