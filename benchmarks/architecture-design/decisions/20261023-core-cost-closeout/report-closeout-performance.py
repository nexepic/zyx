from pathlib import Path
import csv,json,statistics,math,hashlib,tarfile,shutil,subprocess
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-closeout';o=r/'benchmarks/architecture-design/results/20261022-core-cost/closeout';baseline=r/'benchmarks/architecture-design/decisions/20261022-core-cost-p0/baseline.csv'
completion=json.loads((d/'final-completion.json').read_text());assert completion['processes']==99
records=[json.loads(p.read_text()) for p in sorted((o/'workers').glob('*.json'))];assert len(records)==99
base={(v['scale'],v['family'],v['case'],v['engine']):v for v in csv.DictReader(baseline.open())}
final={(v['scale'],v['family'],v['case'],v['engine']):v for v in csv.DictReader((d/'final.csv').open())}
num=lambda row,key:float(row[key]) if row else None
fmt=lambda n: 'N/A' if n is None else f'{n:.3f}'
rows=[]
for key,after in sorted(final.items()):
 scale,family,case,engine=key
 if engine!='zyx':continue
 kfamily='import-equal-reopened' if case=='fresh_import' else family
 before=base.get(key);control=final.get((scale,kfamily,case,'kuzu'));oldcontrol=base.get((scale,kfamily,case,'kuzu'))
 a=num(after,'pooled_p50_ms');b=num(before,'pooled_p50_ms');k=num(control,'pooled_p50_ms')
 rows.append({'scale':scale,'family':family,'case':case,'baseline_zyx_p50_ms':b,'final_zyx_p50_ms':a,'current_kuzu_equivalent_p50_ms':k,'baseline_kuzu_p50_ms':num(oldcontrol,'pooled_p50_ms'),'speedup_vs_baseline':b/a if b is not None else None,'zyx_over_kuzu':a/k if k else None,'baseline_zyx_p95_ms':num(before,'pooled_p95_ms'),'final_zyx_p95_ms':num(after,'pooled_p95_ms'),'current_kuzu_p95_ms':num(control,'pooled_p95_ms'),'samples':int(after['samples']),'p95_interpretation':'3-import maximum; not reliable P95' if case=='fresh_import' else 'pooled30 observations; per-process results also retained'})
with (d/'comparison.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
with (d/'per-process.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['round','scale','family','engine','case','samples','p50_ms','p95_or_import_max_ms','status'])
 for record in records:
  groups={}
  for sample in record['samples']:groups.setdefault(sample['case'],[]).append(sample['latency_ms'])
  for case,values in groups.items():w.writerow([record['round'],record['scale'],record['family'],record['engine'],case,len(values),statistics.median(values),sorted(values)[math.ceil(.95*len(values))-1],record['status']])
with (d/'resources.csv').open('w',newline='') as f:
 fields=['round','scale','family','engine','status','wall_seconds','user_seconds','system_seconds','peak_rss_bytes','os_read_bytes','os_write_bytes','observed_private_file_peak_bytes','private_file_bytes']
 w=csv.DictWriter(f,fieldnames=fields);w.writeheader()
 for rec in records:w.writerow({name:rec.get(name,rec.get('resources',{}).get(name)) for name in fields})
primary=['label_scan_filter','aggregation_count_by_group','multi_key_group','property_set','edge_batch_1000','fresh_import']
gate=[]
for row in rows:
 if row['case'] in primary:
  passed=row['current_kuzu_equivalent_p50_ms'] is not None and row['final_zyx_p50_ms'] <= row['current_kuzu_equivalent_p50_ms']
  gate.append({'scale':row['scale'],'case':row['case'],'Kuzu_P50_passed':passed,'baseline_5x_passed': row['speedup_vs_baseline']>=5 if row['case'] in ['label_scan_filter','aggregation_count_by_group','property_set'] else None})
summary={'measurement_status':completion['status'],'processes':len(records),'failed_processes':[{'scale':v['scale'],'family':v['family'],'engine':v['engine'],'round':v['round'],'failures':v['failures']} for v in records if v['status']!='passed'],'implementation_progress_percent':90,'P7_accepted':False,'Kuzu_objective_met':False,'experimental_cold_adoption':False,'commit_push':False,'primary_gates':gate,'scope_closed':'Final measured outcome retained; no follow-on optimization branches opened. Public cold conversion/whole-target bootstrap remains explicitly undelivered.','baseline_sha256':hashlib.file_digest(baseline.open('rb'),'sha256').hexdigest()}
(d/'closeout.json').write_text(json.dumps(summary,indent=2)+'\n')
lines=['# Final measured comparison / 最终实测对比','','All times are milliseconds. Lower is faster. Speedup is old ZYX/new ZYX; ZYX/Kuzu above1 means ZYX is slower. All99 registered workers are retained. Cold-layout adoption remains disabled; these are the actual default-format production paths.','', '| Dataset | Workload | Before P50 | Final P50 | Kuzu equivalent P50 | Speedup | ZYX/Kuzu |', '|---|---|---:|---:|---:|---:|---:|']
for row in rows:lines.append('| '+' | '.join([row['scale'],row['case'],fmt(row['baseline_zyx_p50_ms']),fmt(row['final_zyx_p50_ms']),fmt(row['current_kuzu_equivalent_p50_ms']),fmt(row['speedup_vs_baseline']),fmt(row['zyx_over_kuzu'])])+' |')
lines+=['','## Kuzu production COPY (different transaction boundary)','', '| Dataset | ZYX production P50 | Kuzu COPY P50 | ZYX/Kuzu |','|---|---:|---:|---:|']
for scale in ['medium','holdout']:
 a=next(x for x in rows if x['scale']==scale and x['case']=='fresh_import')['final_zyx_p50_ms'];k=num(final.get((scale,'import-production','fresh_import','kuzu')),'pooled_p50_ms')
 lines.append(f'| {scale} | {fmt(a)} | {fmt(k)} | {fmt(a/k if k else None)} |')
lines+=['','- Queries/writes:30 observations from three independent processes, three warmups and ten measurements each. See per-process.csv; no slow samples removed.','- Imports:three independent imports. Their maximum is not a reliable P95. The equivalent import control reopens Kuzu at the same1000-row transaction boundary; COPY is displayed separately.','- comparison.csv includes baseline/current P95, sample counts and both old/current Kuzu controls. resources.csv records all process CPU/RSS/OS I/O and observed file peaks. Parent polling can miss short-lived file peaks; do not interpret it as an exact allocation census.','- Large/skew fixtures have no Kuzu-equivalent result and remain N/A; current/prospective support must not be invented.','- Baseline was frozen before these changes; absolute before/after comparisons can include shared-host variation. The controls were rerun with the final candidate and the unchanged registered ordering/limits.','- Linux only. No three-platform or full-unit-suite claim. Historical CPU percentages are not a new profile.','- P7/Kuzu objective remains unmet. Acceptance stays90%; report completion does not earn the last10% or authorize commit/push.','']
(d/'comparison.md').write_text('\n'.join(lines))
# Store every raw worker receipt, oracle output, stderr and all current probe build identities.
with tarfile.open(d/'worker-evidence.tar.gz','w:gz') as t:
 for p in sorted((o/'workers').iterdir()):
  if p.is_file() and p.suffix in ['.json','.jsonl','.stderr']:t.add(p,arcname='workers/'+p.name)
for name in ['build.json','campaign.log']:shutil.copy2(o/name,d/('final-'+name))
print(json.dumps({'processes':len(records),'failed_processes':len(summary['failed_processes']),'primary':[{k:v for k,v in row.items() if k in ['scale','case','baseline_zyx_p50_ms','final_zyx_p50_ms','current_kuzu_equivalent_p50_ms','speedup_vs_baseline','zyx_over_kuzu']} for row in rows if row['case'] in primary]},indent=2))
