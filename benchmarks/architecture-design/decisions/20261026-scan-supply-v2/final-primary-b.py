"""Final primary window. Run only after module and source validation stabilize."""
from pathlib import Path
import json,subprocess,sys,hashlib,statistics,os
from process_receipt import run
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent;B=D/'b-field-cohort';O=B/'final-primary'
assert '--validated-run' in sys.argv, 'Run only after validation; no development benchmarking'
modules=[json.loads((D/'b-cohort-tests/receipt.json').read_text())]
for profile in ['b-cohort-tests','b-cohort-hybrid','b-cohort-row']:
 assert json.loads((D/profile/'receipt.json').read_text())['returncode']==0
assert json.loads((B/'generality-lifecycle-receipt.json').read_text())['returncode']==0
assert json.loads((D/'b-whole-group-deletion-native/receipt.json').read_text())['returncode']==0
cohort_tests=json.loads((D/'b-heterogeneous-spill/cohort-module-receipt.json').read_text())
assert cohort_tests['returncode']==0 and cohort_tests['failures']==0 and cohort_tests['tests']==9
assert json.loads((B/'medium-validation/receipt.json').read_text())['work_gate_passed']
for scale in ['medium','holdout']:
 receipt=json.loads((B/(scale+'-conversion-receipt.json')).read_text())
 assert receipt['resources']['returncode']==0
 assert receipt['source_sha256_before']==receipt['source_sha256_after']
assert not O.exists(), 'Preserve existing formal window; investigate failures narrowly'
O.mkdir()
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
cases=['all_nodes_property_filter','label_scan_filter','aggregation_count_by_group','multi_key_group']
control=campaign.INPUT_ROOT/'s4-native-write-cost/public-b-final';candidate=B/'probes/public';kuzu=campaign.HERE/'kuzu-worker.py'
binaries={'control':control,'candidate':candidate}
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
freeze={'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True,cwd=R).strip(),'zyx_binaries':{k:{'path':str(v),'sha256':sha(v)} for k,v in binaries.items()},'kuzu_worker':{'path':str(kuzu),'sha256':sha(kuzu)},'source_hashes':{},'warmups':3,'iterations':10,'processes_per_engine_dataset':3,'threads':1,'cases':cases}
assert sha(R/'tests/src/storage/relocatable/entity/test_BuildingScanFieldCohorts.cpp')==cohort_tests['source_sha256']
freeze['selected_module_cases']={'scan':61,'hybrid':32,'row':3,'total':96,'aggregation':'original SCAN 59 minus previous cohort 7 plus current cohort 9; no double counting'}
freeze['candidate_probe_recipe']=json.loads((B/'probes/public-link.json').read_text())
source_archive=json.loads((B/'frozen-source-receipt.json').read_text())
assert source_archive['archive_entries_verified'] and sha(B/'frozen-source.tar.gz')==source_archive['archive_sha256']
freeze['source_archive_sha256']=source_archive['archive_sha256']
freeze['registration_sha256']=sha(D.parent/'20261026-scan-supply-v0/acceptance-registration.json')
freeze['measurement_script_sha256']=sha(Path(__file__))
freeze['resource_helper_sha256']=sha(D/'process_receipt.py')
freeze['resource_limits']=campaign.REG['limits']
freeze['cpu_affinity']=[0]
freeze['validation_receipts']={str(p.relative_to(D)):sha(p) for p in [D/'b-cohort-tests/receipt.json',D/'b-cohort-hybrid/receipt.json',D/'b-cohort-row/receipt.json',D/'b-whole-group-deletion-native/receipt.json',D/'b-heterogeneous-spill/cohort-module-receipt.json',B/'generality-lifecycle-receipt.json',B/'medium-validation/receipt.json',B/'medium-conversion-receipt.json',B/'holdout-conversion-receipt.json',B/'source-freeze.json',B/'frozen-source-receipt.json',D/'b-field-cohort-registration.json']}
probe_receipt=json.loads((B/'probes/receipt.json').read_text())
assert sha(candidate)==probe_receipt['binaries']['public']
for scale in ['medium','holdout']:
 assert sha(B/(scale+'.zyx'))==json.loads((B/(scale+'-conversion-receipt.json')).read_text())['target_sha256']
freeze['buffer_pool_bytes']=67108864
freeze['query_materialized_budget_bytes']=67108864
for base in ['src','include']:
 for p in (R/base).rglob('*'):
  if p.is_file() and p.suffix in ['.cpp','.hpp','.h','.inc']:freeze['source_hashes'][str(p.relative_to(R))]=sha(p)
for scale in ['medium','holdout']:
 for p in [campaign.INPUT_ROOT/(scale+'.scan-corrected-v1.zyx'),campaign.INPUT_ROOT/(scale+'.kuzu'),B/(scale+'.zyx')]:freeze[str(p)]=sha(p)
for relative,digest in json.loads((B/'source-freeze.json').read_text())['engine_source_hashes'].items():
 assert sha(R/relative)==digest,'Frozen B source changed before formal window'

assert sha(R/'build-scan-candidate/libzyx_core.a')==modules[0]['library_sha256']
freeze['module_receipt_sha256']=sha(D/'b-cohort-tests/receipt.json')
freeze['lifecycle_receipt_sha256']=sha(B/'generality-lifecycle-receipt.json')
freeze['resource_launcher_sha256']=sha(D/'resource-launcher')
freeze['resource_launcher_source_sha256']=sha(D/'resource-launcher.c')
(O/'freeze.json').write_text(json.dumps(freeze,indent=2)+'\n')
rows=[]
for round_index in range(3):
 for scale in ['medium','holdout']:
  for engine in (['control','candidate','kuzu'] if round_index%2==0 else ['kuzu','candidate','control']):
   stem=O/f'{round_index}-{scale}-{engine}';target=Path('/tmp')/f'zyx-v4-b-field-cohort-primary-{round_index}-{scale}-{engine}.db';assert not target.exists()
   src=(B/(scale+'.zyx') if engine=='candidate' else campaign.INPUT_ROOT/(scale+('.scan-corrected-v1.zyx' if engine=='control' else '.kuzu')))
   cmd=([str(binaries[engine])] if engine!='kuzu' else [campaign.PYTHON,str(kuzu)])+['read',str(src),str(target),','.join(cases),'3','10']
   cmd=['/usr/bin/taskset','-c','0']+cmd
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
 for path in [campaign.INPUT_ROOT/(scale+'.scan-corrected-v1.zyx'),campaign.INPUT_ROOT/(scale+'.kuzu'),B/(scale+'.zyx')]:assert sha(path)==freeze[str(path)],'Input changed during formal window'
for relative,digest in freeze['validation_receipts'].items():assert sha(D/relative)==digest,'Receipt changed during formal window'


assert sha(B/'frozen-source.tar.gz')==freeze['source_archive_sha256'],'Frozen source archive changed during formal window'
