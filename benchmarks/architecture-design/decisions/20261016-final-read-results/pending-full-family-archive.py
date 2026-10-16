from pathlib import Path
from collections import Counter,defaultdict
import json,hashlib,tarfile,shutil,re
r=Path('/workspace/zyx');root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v3')
out=r/'benchmarks/architecture-design/decisions/20261016-s03-complete-versioned-baseline'
def digest(p):
 h=hashlib.sha256()
 with Path(p).open('rb') as f:
  for b in iter(lambda:f.read(1024**2),b''):h.update(b)
 return h.hexdigest()
campaign=json.loads((root/'contract-campaign-completed.json').read_text());cases=campaign['cases'];assert len(cases)==117
assert len({(v['round'],v['engine'],v['family']) for v in cases})==117
families=sorted({v['family'] for v in cases});assert len(families)==13
reg=json.loads((root/'registration.json').read_text());read=json.loads((root/'read-comparison.json').read_text());comparables=[];samples=0;processes=set();distributions=0
for row in read['cases']:
 assert row['current_p50_reduction_vs_control'] is None
 if all('median_process_p50_ms' in row['engines'][e] for e in ['current','kuzu']):comparables.append(row)
 for e in ['current','kuzu']:
  for p in row['engines'][e].get('processes',{}).values():samples+=p['samples'];processes.add(p['source']);distributions+=1
assert (len(comparables),samples,len(processes),distributions)==(20,1200,24,120)
out.mkdir(exist_ok=False);files=[];rows=[];by_family=defaultdict(list)
for case in cases:
 folder=root/'contract'/f"round-{case['round']}"/case['engine']/case['family'];by_family[case['family']].append(case)
 if (folder/'registration.json').exists() and case['engine']!='kuzu':
  actual=json.loads((folder/'registration.json').read_text())['worker_sha256'];original='/workspace/zyx/buildDir_perf_control/zyx-workload-contract-worker' if case['engine']=='current' else '/workspace/zyx-optimization-control-20261015/binaries/zyx-workload-contract-worker';assert actual==reg['binaries'][original]
 if case['status']=='completed':
  report=json.loads((folder/'report.json').read_text());assert report['full_results_checked'] and report['families']==[case['family']]
 for p in folder.rglob('*'):
  if p.is_file():files.append(p)
 log=Path(case.get('log',''))
 if log.is_file():files.append(log)
for family in families:
 row={'family':family,'outcomes':{e:dict(Counter(v['status'] for v in by_family[family] if v['engine']==e)) for e in ['control','current','kuzu']},'failed_messages':[]}
 for case in by_family[family]:
  if case['status'] in ['failed','inconclusive_timeout']:
   log=Path(case['log']);lines=log.read_text(errors='replace').splitlines();row['failed_messages'].append({'engine':case['engine'],'round':case['round'],'status':case['status'],'last_lines':lines[-5:]})
 rows.append(row)
for name in ['registration.json','frozen-baseline-continuation.json','read-campaign-completed.json','read-comparison.json','read-comparison.csv','contract-campaign-completed.json','paused-for-maintenance-correctness.json']:
 p=root/name
 if p.exists():shutil.copy2(p,out/name)
files=sorted(set(files));manifest=[]
for p in files:manifest.append({'path':str(p.relative_to(root)),'bytes':p.stat().st_size,'sha256':digest(p)})
archive=out/'full-family-evidence.tar.gz'
with tarfile.open(archive,'w:gz',compresslevel=6) as tf:
 for p in files:tf.add(p,arcname=str(p.relative_to(root)),recursive=False)
with tarfile.open(archive,'r:gz') as tf:
 for m in manifest:
  f=tf.extractfile(m['path']);h=hashlib.sha256()
  for b in iter(lambda:f.read(1024**2),b''):h.update(b)
  assert h.hexdigest()==m['sha256']
(out/'full-family-files.json').write_text(json.dumps({'archive':archive.name,'archive_sha256':digest(archive),'files':manifest,'decompressed_hash_verification':'passed'},indent=2)+'\n')
(out/'family-outcomes.json').write_text(json.dumps({'baseline_source_commit':'6a735df86a678556aa71877b6aa66fe968292df7','families':rows,'status_counts':dict(Counter(v['status'] for v in cases)),'nominal_case_slots':117,'executed_independent_processes':sum(v['status'] in ['completed','failed','inconclusive_timeout'] for v in cases),'nonmaintenance_timing':'Full-size semantic/operational observations; not repeated warm-query distributions or performance gates.','formal_read_comparable_workloads':20,'formal_read_samples':1200,'formal_read_independent_profile_processes':24,'formal_read_process_workload_distributions':120},indent=2)+'\n')
print(json.dumps({'out':str(out),'files':len(files),'archive_bytes':archive.stat().st_size,'status_counts':dict(Counter(v['status'] for v in cases))},indent=2))
