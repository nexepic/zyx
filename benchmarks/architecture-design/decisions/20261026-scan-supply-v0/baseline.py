"""One frozen initial primary-query comparison; no guard/full-suite invocation."""
from pathlib import Path
import json,subprocess,sys,hashlib,statistics,os
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent;O=D/'baseline';O.mkdir(exist_ok=True)
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
cases=['all_nodes_property_filter','label_scan_filter','aggregation_count_by_group','multi_key_group']
zyx=campaign.INPUT_ROOT/'s4-native-write-cost/public-b-final';kuzu=campaign.HERE/'kuzu-worker.py'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
freeze={'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=R).strip(),'zyx_binary':{'path':str(zyx),'sha256':sha(zyx)},'kuzu_worker':{'path':str(kuzu),'sha256':sha(kuzu)},'source_hashes':{},'warmups':3,'iterations':10,'processes_per_engine_dataset':3,'threads':1,'cases':cases}
for base in ['src','include']:
 for p in (R/base).rglob('*'):
  if p.is_file() and p.suffix in ['.cpp','.hpp','.h','.inc']:freeze['source_hashes'][str(p.relative_to(R))]=sha(p)
for scale in ['medium','holdout']:
 for suffix in ['.scan-corrected-v1.zyx','.kuzu']:
  p=campaign.INPUT_ROOT/(scale+suffix);freeze[str(p)]=sha(p)
(O/'freeze.json').write_text(json.dumps(freeze,indent=2)+'\n')
rows=[]
for round_index in range(3):
 for scale in ['medium','holdout']:
  for engine in (['zyx','kuzu'] if round_index%2==0 else ['kuzu','zyx']):
   stem=O/f'{round_index}-{scale}-{engine}';target=Path('/tmp')/f'zyx-v0-baseline-{round_index}-{scale}-{engine}.db';assert not target.exists()
   src=campaign.INPUT_ROOT/(scale+('.scan-corrected-v1.zyx' if engine=='zyx' else '.kuzu'))
   cmd=([str(zyx)] if engine=='zyx' else [campaign.PYTHON,str(kuzu)])+['read',str(src),str(target),','.join(cases),'3','10']
   print('START',round_index,scale,engine,flush=True)
   with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:p=subprocess.run(cmd,stdout=out,stderr=err,env=campaign.ENV,preexec_fn=campaign.limits,timeout=1200)
   events=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert p.returncode==0 and len(events)==40,(p.returncode,len(events))
   for e in events:campaign.check_event(e['case'],e,scale,False);rows.append(dict(e,engine=engine,scale=scale,round=round_index))
   stem.with_suffix('.receipt.json').write_text(json.dumps({'command':cmd,'oracle':'passed','samples':len(events)},indent=2)+'\n');print('PASS',round_index,scale,engine,flush=True)
summary=[]
for scale in ['medium','holdout']:
 for case in cases:
  stat={}
  for engine in ['zyx','kuzu']:
   xs=sorted(e['latency_ms'] for e in rows if e['engine']==engine and e['case']==case and e['scale']==scale);q=.95*(len(xs)-1);i=int(q)
   stat[engine]={'p50_ms':statistics.median(xs),'p95_ms':xs[i]+(xs[min(i+1,len(xs)-1)]-xs[i])*(q-i),'n':len(xs),'process_p50_ms':[statistics.median(e['latency_ms'] for e in rows if e['engine']==engine and e['case']==case and e['scale']==scale and e['round']==rnd) for rnd in range(3)]}
  summary.append({'scale':scale,'case':case,**stat,'zyx_over_kuzu':stat['zyx']['p50_ms']/stat['kuzu']['p50_ms']})
(O/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print('COMPLETE',len(rows),'oracle validated samples',flush=True)
