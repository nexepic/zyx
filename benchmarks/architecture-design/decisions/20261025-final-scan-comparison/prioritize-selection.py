"""Finish registered primary samples before spending more time on S5 guards.

The original orchestrator is paused externally. Its current timed child must
finish first. This controller reuses its exact frozen worker and receipt names;
completed primary processes are loaded, never repeated.
"""
from pathlib import Path
import csv,hashlib,json,math,statistics,sys,time
HERE=Path(__file__).resolve().parent
ROOT=Path('/workspace/zyx/benchmarks/architecture-design/results/20261024-work-scan/final-comparison')
priority=json.loads((ROOT/'selection-priority.json').read_text())
def state(pid):
 try:return Path(f'/proc/{pid}/stat').read_text().split(') ',1)[1].split()[0]
 except FileNotFoundError:return None
assert state(priority['parent_pid']) in ['T','t'], 'original orchestrator must remain paused'
for child in priority['active_worker']:
 while state(child['pid']) not in [None,'Z']:
  time.sleep(.2)
print('active guard worker finished; frozen primary selection resumes',flush=True)
import campaign
freeze_path=ROOT/'freeze.json';freeze=json.loads(freeze_path.read_text())
campaign.FREEZE_ID=hashlib.sha256(freeze_path.read_bytes()).hexdigest()
for relative,expected in json.loads((ROOT/'source-files.json').read_text()).items():
 assert hashlib.file_digest((campaign.REPO/relative).open('rb'),'sha256').hexdigest()==expected,relative
for name,entry in freeze['artifacts'].items():
 assert hashlib.file_digest(Path(entry['path']).open('rb'),'sha256').hexdigest()==entry['sha256'],name
campaign.B_IMAGES.update({scale:Path(freeze['artifacts'][scale+'_scan']['path']) for scale in ['medium','holdout']})
cases=['label_scan_filter','all_nodes_property_filter','aggregation_count_by_group','multi_key_group']
for n in range(3):
 order=['initial','control','A','B','kuzu'] if n%2==0 else ['kuzu','B','A','control','initial']
 for scale in ['medium','holdout']:
  for engine in order:campaign.worker(engine,scale,'scan',cases,n)
assert len(campaign.results)==30
rows={}
for record in campaign.results:
 for event in record['samples']:rows.setdefault((record['scale'],event['case'],record['engine']),[]).append(event['latency_ms'])
summary={key:{'samples':len(values),'p50_ms':statistics.median(values),'p95_ms':sorted(values)[math.ceil(.95*len(values))-1]} for key,values in rows.items()}
with (ROOT/'selection-summary.csv').open('w',newline='') as f:
 writer=csv.writer(f);writer.writerow(['scale','case','engine','samples','p50_ms','p95_ms'])
 for key,stats in sorted(summary.items()):writer.writerow([*key,stats['samples'],stats['p50_ms'],stats['p95_ms']])
gates=[]
for scale in ['medium','holdout']:
 for case in cases:
  for candidate in ['A','B']:
   row=summary.get((scale,case,candidate));control={name:summary.get((scale,case,name)) for name in ['initial','control']};kuzu=summary.get((scale,case,'kuzu'))
   valid=bool(row and kuzu and all(control.values()) and all(item['samples']==30 for item in [row,kuzu,*control.values()]))
   speedups={name:item['p50_ms']/row['p50_ms'] for name,item in control.items()} if valid else {}
   gates.append({'scale':scale,'case':case,'candidate':candidate,'valid':valid,'speedup':speedups,'passes_5x_each_control':valid and all(x>=5 for x in speedups.values()),'candidate_p50_ms':row['p50_ms'] if row else None,'kuzu_p50_ms':kuzu['p50_ms'] if kuzu else None,'beats_kuzu':valid and row['p50_ms']<kuzu['p50_ms']})
report={'freeze_sha256':campaign.FREEZE_ID,'primary_processes':len(campaign.results),'primary_samples':sum(len(r['samples']) for r in campaign.results),'primary':gates,'candidate_passes_all_primary':{candidate:all(g['passes_5x_each_control'] for g in gates if g['candidate']==candidate) for candidate in ['A','B']},'s5_status':'paused at the unaccepted S2 selection gate; guards are not declared complete','changed_engine_code_or_primary_sample_count':False}
(ROOT/'selection-gate.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'candidate_passes_all_primary':report['candidate_passes_all_primary'],'primary_processes':report['primary_processes'],'primary_samples':report['primary_samples']}),flush=True)
