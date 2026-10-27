"""Final primary window. Run only after module and source validation stabilize."""
from pathlib import Path
import json,subprocess,sys,hashlib,statistics,os
from process_receipt import run
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent;O=D/'final-primary-a2'
assert '--validated-run' in sys.argv, 'Run only after validation; no development benchmarking'
modules=json.loads((D/'presence-module-receipt.json').read_text())
assert len(modules)==2 and all(x['failures']==0 for x in modules)
assert json.loads((D/'generality-presence-receipt.json').read_text())['returncode']==0
assert not O.exists(), 'Preserve existing formal window; investigate failures narrowly'
O.mkdir()
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
cases=['all_nodes_property_filter','label_scan_filter','aggregation_count_by_group','multi_key_group']
control=campaign.INPUT_ROOT/'s4-native-write-cost/public-b-final';candidate=D/'public-a2-final';kuzu=campaign.HERE/'kuzu-worker.py'
binaries={'control':control,'candidate':candidate}
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
freeze={'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=R).strip(),'zyx_binaries':{k:{'path':str(v),'sha256':sha(v)} for k,v in binaries.items()},'kuzu_worker':{'path':str(kuzu),'sha256':sha(kuzu)},'source_hashes':{},'warmups':3,'iterations':10,'processes_per_engine_dataset':3,'threads':1,'cases':cases}
freeze['candidate_probe_recipe']=json.loads((D/'public-a2-final-link.json').read_text())
freeze['registration_sha256']=sha(D.parent/'20261026-scan-supply-v0/acceptance-registration.json')
freeze['measurement_script_sha256']=sha(Path(__file__))
freeze['resource_helper_sha256']=sha(D/'process_receipt.py')
freeze['resource_limits']=campaign.REG['limits']
freeze['buffer_pool_bytes']=67108864
freeze['query_materialized_budget_bytes']=67108864
for base in ['src','include']:
 for p in (R/base).rglob('*'):
  if p.is_file() and p.suffix in ['.cpp','.hpp','.h','.inc']:freeze['source_hashes'][str(p.relative_to(R))]=sha(p)
for scale in ['medium','holdout']:
 for suffix in ['.scan-corrected-v1.zyx','.kuzu']:
  p=campaign.INPUT_ROOT/(scale+suffix);freeze[str(p)]=sha(p)
assert sha(R/'build-scan-candidate/libzyx_core.a')==next(x['library_sha256'] for x in modules if x['profile']=='scan')
freeze['module_receipt_sha256']=sha(D/'presence-module-receipt.json')
freeze['lifecycle_receipt_sha256']=sha(D/'generality-presence-receipt.json')
freeze['resource_launcher_sha256']=sha(D/'resource-launcher')
freeze['resource_launcher_source_sha256']=sha(D/'resource-launcher.c')
(O/'freeze.json').write_text(json.dumps(freeze,indent=2)+'\n')
rows=[]
for round_index in range(3):
 for scale in ['medium','holdout']:
  for engine in (['control','candidate','kuzu'] if round_index%2==0 else ['kuzu','candidate','control']):
   stem=O/f'{round_index}-{scale}-{engine}';target=Path('/tmp')/f'zyx-v4-a2-primary-{round_index}-{scale}-{engine}.db';assert not target.exists()
   src=campaign.INPUT_ROOT/(scale+('.scan-corrected-v1.zyx' if engine!='kuzu' else '.kuzu'))
   cmd=([str(binaries[engine])] if engine!='kuzu' else [campaign.PYTHON,str(kuzu)])+['read',str(src),str(target),','.join(cases),'3','10']
   print('START',round_index,scale,engine,flush=True)
   with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:resources=run(cmd,out,err,campaign.ENV,campaign.REG['limits'])
   stem.with_suffix('.resources.json').write_text(json.dumps(resources,indent=2)+'\n')
   events=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert resources['returncode']==0 and len(events)==40,(resources,len(events))
   for e in events:campaign.check_event(e['case'],e,scale,False);rows.append(dict(e,engine=engine,scale=scale,round=round_index))
   stem.with_suffix('.receipt.json').write_text(json.dumps({'command':cmd,'oracle':'passed','samples':len(events),'resources':resources},indent=2)+'\n');print('PASS',round_index,scale,engine,flush=True)
summary=[]
for scale in ['medium','holdout']:
 for case in cases:
  stat={}
  for engine in ['control','candidate','kuzu']:
   xs=sorted(e['latency_ms'] for e in rows if e['engine']==engine and e['case']==case and e['scale']==scale);q=.95*(len(xs)-1);i=int(q)
   stat[engine]={'p50_ms':statistics.median(xs),'p95_ms':xs[i]+(xs[min(i+1,len(xs)-1)]-xs[i])*(q-i),'n':len(xs),'process_p50_ms':[statistics.median(e['latency_ms'] for e in rows if e['engine']==engine and e['case']==case and e['scale']==scale and e['round']==rnd) for rnd in range(3)]}
  summary.append({'scale':scale,'case':case,**stat,'candidate_over_kuzu':stat['candidate']['p50_ms']/stat['kuzu']['p50_ms'],'control_over_candidate':stat['control']['p50_ms']/stat['candidate']['p50_ms']})
(O/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print('COMPLETE',len(rows),'oracle validated samples',flush=True)

import math
speedups=[x['control_over_candidate'] for x in summary]
gain=math.exp(sum(map(math.log,speedups))/len(speedups))
all_node=[x['control_over_candidate'] for x in summary if x['case']=='all_nodes_property_filter']
(O/'gates.json').write_text(json.dumps({'primary_geomean_speedup':gain,'gain_gate_passed':gain>=2 and all(x>=2 for x in all_node),'kuzu_p50_target_passed':all(x['candidate_over_kuzu']<=1 for x in summary),'guards_and_lifecycle_required_separately':True},indent=2)+'\n')
for name,binary in binaries.items():assert sha(binary)==freeze['zyx_binaries'][name]['sha256'], 'Binary changed during formal window'
for relative,digest in freeze['source_hashes'].items():assert sha(R/relative)==digest, 'Source changed during formal window'

for scale in ['medium','holdout']:
 for suffix in ['.scan-corrected-v1.zyx','.kuzu']:
  path=campaign.INPUT_ROOT/(scale+suffix);assert sha(path)==freeze[str(path)], 'Input changed during formal window'
