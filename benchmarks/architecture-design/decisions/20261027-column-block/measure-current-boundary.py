"""R0 medium-only attribution; production core is unchanged; not acceptance timing."""
from pathlib import Path
import json,hashlib,sys,statistics
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;O=N/'current-boundary';D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort'
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
sha=lambda p:hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
assert (O/'build-receipt.json').exists() and not (O/'protocol.json').exists()
cases=['all_nodes_property_filter','label_scan_filter','multi_key_group'];source=B/'medium.zyx'
protocol={'cases':cases,'scope':'Current B core and B image; medium only, no holdout tuning. Alternating paired enabled/disabled boundary observations, not a new performance acceptance window.','warmups':4,'iterations':80,'sampled_per_case':40,'control_per_case':40,'perturbation_max_fraction':.05,'cpu_wall_max_fraction':.05,'tsc_calibration_max_fraction':.01,'input_sha256':sha(source),'probe_sha256':sha(O/'probe'),'library_sha256':sha(R/'build-scan-candidate/libzyx_core.a'),'limits':campaign.REG['limits'],'cpu_affinity':[0],'runner_sha256':sha(__file__)}
(O/'protocol.json').write_text(json.dumps(protocol,indent=2)+'\n');summary=[]
for case in cases:
 stem=O/case;target='/tmp/zyx-current-boundary-'+case+'.db'
 cmd=['/usr/bin/taskset','-c','0',str(O/'probe'),'read',str(source),target,case,'4','80']
 print('START',case,flush=True)
 with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:resources=run(cmd,out,err,dict(campaign.ENV,ZYX_CPU_SAMPLE_ENABLE='1'),campaign.REG['limits'])
 stem.with_suffix('.resources.json').write_text(json.dumps(resources,indent=2)+'\n');assert resources['returncode']==0,resources
 rows=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(rows)==80
 for x in rows:campaign.check_event(case,x,'medium',False)
 observed=[x for x in rows if x['sampled']];control=[x for x in rows if not x['sampled']];assert len(observed)==len(control)==40
 assert all(x['source_calls']>0 and 0<=x['callback_ns']<=x['source_ns'] for x in observed)
 med=lambda field:statistics.median(x[field] for x in observed)
 ratio=med('latency_ms')/statistics.median(x['latency_ms'] for x in control)
 cpu=statistics.median((x['user_ms']+x['system_ms'])/x['latency_ms'] for x in observed)
 calibration=[json.loads(l)['tsc_calibration_ratio'] for l in stem.with_suffix('.stderr').read_text().splitlines() if l.startswith('{"tsc_calibration_ratio"')];assert len(calibration)==1
 record={'case':case,'oracles':80,'sampled_control_median_ratio':ratio,'cpu_wall_ratio':cpu,'tsc_calibration_ratio':calibration[0],'quality_passed':abs(ratio-1)<=.05 and abs(cpu-1)<=.05 and abs(calibration[0]-1)<=.01,'control_p50_ms':statistics.median(x['latency_ms'] for x in control),'observed_p50_ms':med('latency_ms'),'source_inclusive_ms':med('source_ns')/1e6,'callbacks_ms':med('callback_ns')/1e6,'exclusive_source_elapsed_share':statistics.median((x['source_ns']-x['callback_ns'])/(x['latency_ms']*1e6) for x in observed),'source_calls':med('source_calls'),'callbacks':med('callback_calls'),'interpretation':'Only qualified boundary elapsed share; not exact CPU percentages. Outside-boundary work includes membership supply, higher adapters and operators.'}
 stem.with_suffix('.receipt.json').write_text(json.dumps(dict(record,command=cmd),indent=2)+'\n');summary.append(record);print('RESULT',json.dumps(record),flush=True)
assert sha(source)==protocol['input_sha256'];assert sha(R/'build-scan-candidate/libzyx_core.a')==protocol['library_sha256']
(O/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print('COMPLETE240 full oracles; rejected observations retained',flush=True)
