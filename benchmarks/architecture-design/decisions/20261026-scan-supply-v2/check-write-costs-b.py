"""Untimed query-only write/resource counters; never acceptance latency samples."""
from pathlib import Path
import hashlib,json,subprocess,sys
from process_receipt import run
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent;B=D/'b-field-cohort';O=B/'write-costs'
assert json.loads((B/'final-primary/gates.json').read_text())['gain_gate_passed']
assert not O.exists();O.mkdir()
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
binaries={'control':campaign.INPUT_ROOT/'s4-native-write-cost/values-b-final','candidate':B/'probes/values'}
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
summary=[]
for scale in ['medium','holdout']:
 inputs={'control':campaign.INPUT_ROOT/(scale+'.scan-corrected-v1.zyx'),'candidate':B/(scale+'.zyx')};before={k:sha(p) for k,p in inputs.items()}
 for case in ['property_set','property_noop','edge_batch_1000']:
  record={'scale':scale,'case':case,'logical_fixed_width_input_bytes':24000 if case.startswith('edge_') else 8 if case=='property_set' else 0}
  for engine,binary in binaries.items():
   source=inputs[engine]
   stem=O/f'{scale}-{case}-{engine}';target=Path('/tmp')/('zyx-v4-b-field-cohort-write-work-'+stem.name+'.db');assert not target.exists()
   cmd=[str(binary),'check-write',str(source),str(target),case,'0','1']
   with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:resources=run(cmd,out,err,campaign.ENV,campaign.REG['limits'])
   assert resources['returncode']==0,(stem,resources)
   events=[json.loads(x) for x in stem.with_suffix('.jsonl').read_text().splitlines() if x.startswith('{')];assert len(events)==1
   campaign.check_event(case,events[0],scale,True)
   values={x['counter']:x for x in (json.loads(s) for s in stem.with_suffix('.stderr').read_text().splitlines() if s.startswith('{'))}
   record[engine]={'completed_storage_write_bytes':values.get('storage_io.completed_write_bytes',{}).get('value',0),'largest_materialized_account_peak_bytes':values.get('logical_plan.materialized_budget_peak_bytes',{}).get('maximum'),'binary_sha256':sha(binary)}
   stem.with_suffix('.receipt.json').write_text(json.dumps({'command':cmd,'complete_oracle':'passed','timing_benchmark':False,'source_sha256':before[engine],'resources':resources},indent=2)+'\n')
  summary.append(record)
 for engine,path in inputs.items():assert sha(path)==before[engine]
(O/'summary.json').write_text(json.dumps({'scope':'Engine completed-write counters within mutation only, excluding private copy/open/postcondition queries; not physical media writes. Peak is maximum observed individual materialized account, not process RSS. Edge logical input is 1000 fixed-width source/target/weight triples; property set is one double. No-op has no changed payload.','rows':summary},indent=2)+'\n')
print('PASS: 12 complete write/postcondition oracles, untimed engine-write counters',flush=True)
