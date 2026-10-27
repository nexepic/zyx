"""Sample linked calls in the unchanged production core; strictly gated targeted attribution."""
from pathlib import Path
import json,sys,statistics,hashlib
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;O=N/'call-cost';D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort';control=N/'current-boundary/probe'
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
assert not (O/'protocol.json').exists()
protocol={'cycles':2,'orders':[['control','instrumented'],['instrumented','control']],'case':'all_nodes_property_filter','iterations':100,'warmups':4,'sampling':'Every 32nd call per function; query-dependent offset; each observed query records total calls, selected calls and their elapsed time inside/outside source. Nested call costs are not additive.','quality_max_fraction':.05,'calibration_max_fraction':.01,'minimum_samples_per_function':100,'scope':'Diagnostic unchanged core. Both cycles and both on/off and linked-control gates must pass; 0.10 maximum difference in estimated per-query fraction. Not an acceptance benchmark.','core_sha256':sha(R/'build-scan-candidate/libzyx_core.a'),'input_sha256':sha(B/'medium.zyx'),'probe_sha256':sha(O/'probe'),'control_sha256':sha(control)}
(O/'protocol.json').write_text(json.dumps(protocol,indent=2)+'\n');results=[]
for cycle,order in enumerate(protocol['orders']):
 rows={}
 for name in order:
  stem=O/(str(cycle)+'-'+name);probe=control if name=='control' else O/'probe';cmd=['/usr/bin/taskset','-c','0',str(probe),'read',str(B/'medium.zyx'),f'/tmp/zyx-call-cost-{cycle}-{name}.db',protocol['case'],'4','100'];print('START',cycle,name,flush=True)
  with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:res=run(cmd,out,err,dict(campaign.ENV,ZYX_CPU_SAMPLE_ENABLE='1'),campaign.REG['limits'])
  stem.with_suffix('.resources.json').write_text(json.dumps(res,indent=2)+'\n');assert res['returncode']==0,res
  rows[name]=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(rows[name])==100
  for x in rows[name]:campaign.check_event(x['case'],x,'medium',False)
 observed=[x for x in rows['instrumented'] if x['sampled']];disabled=[x for x in rows['instrumented'] if not x['sampled']];baseline=[x for x in rows['control'] if not x['sampled']];assert len(observed)==len(disabled)==len(baseline)==50
 median=lambda seq,key:statistics.median(x[key] for x in seq)
 onoff=median(observed,'latency_ms')/median(disabled,'latency_ms');external=median(disabled,'latency_ms')/median(baseline,'latency_ms');cpu=statistics.median((x['user_ms']+x['system_ms'])/x['latency_ms'] for x in observed)
 lines=(O/(str(cycle)+'-instrumented.stderr')).read_text().splitlines();callrows={r['call_ordinal']:r for l in lines if l.startswith('{"call_ordinal"') for r in [json.loads(l)]};cal=[json.loads(l)['tsc_calibration_ratio'] for l in lines if l.startswith('{"tsc_calibration_ratio"')];assert len(cal)==1
 selected=[callrows[x['iteration']+5] for x in observed];times=median(observed,'latency_ms')*1e6;costs=[]
 for i,name in enumerate(['fstat','crc32','query_guard_checkSlow']):
  totals=[sum(r['items'][i][j] for r in selected) for j in range(6)]
  count,inside,samples,inside_samples,ns,inside_ns=totals
  costs.append({'function':name,'total_calls_per_query':count/len(observed),'inside_source_calls_per_query':inside/len(observed),'samples':samples,'inside_samples':inside_samples,'estimated_total_query_fraction':(ns/samples*count/len(observed)/times) if samples else None,'estimated_inside_query_fraction':(inside_ns/inside_samples*inside/len(observed)/times) if inside_samples else None})
 quality=abs(onoff-1)<=.05 and abs(external-1)<=.05 and abs(cpu-1)<=.05 and abs(cal[0]-1)<=.01 and all(x['samples']>=100 for x in costs)
 record={'cycle':cycle,'oracles':200,'quality_passed':quality,'onoff_ratio':onoff,'linked_control_ratio':external,'cpu_wall_ratio':cpu,'calibration_ratio':cal[0],'costs':costs};results.append(record);print('CYCLE',json.dumps(record),flush=True)
passed=all(x['quality_passed'] for x in results) and all(abs(a['estimated_total_query_fraction']-b['estimated_total_query_fraction'])<=.1 for a,b in zip(results[0]['costs'],results[1]['costs']))
assert sha(B/'medium.zyx')==protocol['input_sha256'] and sha(R/'build-scan-candidate/libzyx_core.a')==protocol['core_sha256']
(O/'summary.json').write_text(json.dumps({'quality_passed':passed,'cycles':results},indent=2)+'\n');print('COMPLETE',passed,flush=True)
