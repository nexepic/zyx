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
old_eval=json.loads((OLD/'evaluation.json').read_text());primary=old_eval['primary'];guards=[]
for g in old_eval['guards']:
 g=dict(g);scale,case,engine=g['scale'],g['case'],g['engine'];candidate=stats[(scale,case,engine)]
 ratios={base:{metric:candidate[metric]/stats[(scale,case,base)][metric] for metric in ['p50_ms','p95_ms']} for base in ['initial','control']}
 guards.append({'scale':scale,'case':case,'engine':engine,'ratios':ratios,'exceeds_five_percent_review_threshold':any(v>1.05 for r in ratios.values() for v in r.values()),'evidence':'targeted correction' if engine=='B' and case in ['edge_batch_1000','edge_create_delete'] else 'original final window'})
report={'roadmap_progress_percent':100,'performance_superiority_claimed':False,'beats_kuzu':False,'fixed_5x_required':False,'kuzu_superiority_required_for_roadmap_delivery':False,'original_window':{'processes':171,'passed':162,'failed':9,'validated_samples':3390,'freeze_sha256':sha(OLD/'freeze.json')},'targeted_correction':{'processes':33,'passed':33,'samples':510,'freeze_sha256':sha(NEW/'freeze.json')},'reconciled_evidence':{'processes':171,'validated_samples':3660,'all_complete_oracles_passed':True,'versions':'Explicit composite: original measured read and unchanged guard binaries plus targeted final write binaries and versioned Skew fixture. This is not a new single-binary full campaign.'},'replacement_provenance':provenance,'primary':primary,'guards':guards,'all_performance_guards_passed':not any(g['exceeds_five_percent_review_threshold'] for g in guards),'default_format':'5/31 ROW; unchanged automatic adoption policy','experimental_format':'15/41 explicit offline conversion; evaluate scan reuse and point/write costs before opting in','space':{'medium':{'row_bytes':309985280,'scan_bytes':256507904,'saving_percent':100*(1-256507904/309985280)},'holdout':{'row_bytes':259915776,'scan_bytes':222035968,'saving_percent':100*(1-222035968/259915776)}},'validation_platforms_executed':['Linux x86_64 clang 19'],'platforms_not_executed':['macOS','Windows'],'full_unit_suite_run':False}
(HERE/'report.json').write_text(json.dumps(report,indent=2)+'\n')
lines=['# Roadmap delivery and measured limits','','The implementation roadmap is complete under the user\'s revised completion criterion. Kuzu remains faster in all eight primary read comparisons. Some native point/property and fallback guard measurements exceed the 5% review threshold; this report does not label those guards as passes. The new format remains explicit and experimental, with the default ROW path unchanged.','','## Read comparison (original final window)','','All figures are P50 milliseconds from the same original window; write-only corrections were not represented as new read timings.','','| Dataset | Case | Initial ZYX | Control ZYX | Selected scan path | Kuzu |','|---|---|---:|---:|---:|---:|']
for p in primary:
 scale,case=p['scale'],p['case'];lines.append('| '+scale+' | '+case+' | '+' | '.join(f"{stats[(scale,case,e)]['p50_ms']:.3f}" for e in ['initial','control','B','kuzu'])+' |')
lines+=['','## Validation and scope','','The original 171-process window retained nine failed Skew workers. Independent graph inspection proved that their input contained 32 nodes and no edges. A fresh, versioned copy restores the preregistered 512 edges; the oracle is unchanged. The 33-process correction window replaces those nine workers and 24 affected write workers. Reconciled evidence contains 171 successful process records and 3660 validated samples, with explicit per-record provenance and the original failures retained.','','The final three engine edits affect write placement/publication only. The original read timings identify their archived binaries and c8b37bc3 source; the correction identifies its own production binaries, source freeze and input hashes. No entire performance matrix or full unit suite was repeated.','','See `report.json` and `summary.csv` for P50/P95, guard ratios, frozen identities, and adoption limits. See the sibling S4 validation directory for module results, complete public oracles, old-pin and 4 KiB-cache generality checks, and before/after regression tests.','','## Adoption costs and future work','','Steady whole-file savings are 17.25% on medium and 14.57% on holdout, including the admitted images\' retained structures. Original conversion costs remain about 207 and 136 minutes respectively; conversion is not free. Historical file polling is an observed lower-bound peak, and process RSS high-water marks are not exact allocator peaks. No DRAM-traffic or hardware-stall claims are inferred from sizeof counters or nested timers.','','Known point/property overheads and the substantial Kuzu gap remain future optimization work. Use the scan format for a measured scan-heavy workload that amortizes conversion and accepts the documented point/write costs; retain ordinary ROW for workloads that do not. Large/Skew native conversion refusals remain explicit N/A. Linux was executed; macOS and Windows were not executed in this environment.']
(HERE/'README.md').write_text('\n'.join(lines)+'\n')
print('Reconciled 171 passing records / 3660 samples; Kuzu and guard limits reported honestly')
