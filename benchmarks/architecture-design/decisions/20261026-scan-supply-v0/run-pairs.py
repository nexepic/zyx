"""Question-driven observer quality check; never a formal comparison campaign."""
from pathlib import Path
import subprocess, os, json, hashlib, statistics, sys
R=Path('/workspace/zyx'); D=Path(__file__).resolve().parent
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'))
import campaign
source=campaign.INPUT_ROOT/'medium.scan-corrected-v1.zyx'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
identity={'input':sha(source),'library':sha(R/'build-scan-candidate/libzyx_core.a'),'probe':sha(D/'probe')}
(D/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
results=[]
for round_index in range(2):
 for sampled in ([False,True] if round_index==0 else [True,False]):
  stem=D/f'all-nodes-{round_index}-{sampled}';target=Path('/tmp')/f'zyx-v0-{round_index}-{sampled}.zyx'
  assert not target.exists()
  env=dict(os.environ,ZYX_CPU_SAMPLE_OUTPUT=str(stem.with_suffix('.pcs')));env.pop('ZYX_CPU_SAMPLE_ENABLE',None)
  if sampled:env['ZYX_CPU_SAMPLE_ENABLE']='1'
  cmd=[str(D/'probe'),'read',str(source),str(target),'all_nodes_property_filter','3','10']
  with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:
   proc=subprocess.run(cmd,stdout=out,stderr=err,env=env,timeout=1200)
  events=[json.loads(x) for x in stem.with_suffix('.jsonl').read_text().splitlines()]
  assert proc.returncode==0 and len(events)==10,(proc.returncode,len(events))
  for event in events:campaign.check_event(event['case'],event,'medium',False)
  record={'round':round_index,'sampled':sampled,'median_ms':statistics.median(e['latency_ms'] for e in events),'oracle':'passed','command':cmd}
  results.append(record);(D/'pairs.json').write_text(json.dumps(results,indent=2)+'\n');print(record,flush=True)
assert sha(source)==identity['input']
ratios=[]
for i in range(2):
 a={x['sampled']:x['median_ms'] for x in results if x['round']==i};ratios.append(a[True]/a[False])
(D/'quality.json').write_text(json.dumps({'paired_ratios':ratios,'within_five_percent':all(abs(x-1)<=.05 for x in ratios),'formal_performance':False},indent=2)+'\n')
