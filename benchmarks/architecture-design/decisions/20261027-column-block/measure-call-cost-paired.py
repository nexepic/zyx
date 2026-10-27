"""Short alternating process pairs reduce confounding from process-to-process drift."""
from pathlib import Path
import json,sys,statistics,hashlib
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;O=N/'call-cost-paired';O.mkdir(exist_ok=False);P=N/'call-cost';C=N/'current-boundary/probe';D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort'
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
protocol={'pairs':6,'orders':[['control','instrumented'] if i%2==0 else ['instrumented','control'] for i in range(6)],'case':'all_nodes_property_filter','iterations':16,'warmups':4,'limits':campaign.REG['limits'],'source_sha256':sha(B/'medium.zyx'),'core_sha256':sha(R/'build-scan-candidate/libzyx_core.a'),'probe_sha256':sha(P/'probe'),'control_sha256':sha(C),'quality_rule':'Absolute deviation of median pairwise disabled/control ratio from 1 <= 5%; median absolute deviation of pairwise ratios <= 5 percentage points; absolute deviation of pooled enabled/disabled P50 from 1 <= 5%; CPU/wall <= 5%, TSC calibration <= 1%; at least 100 sampled calls per function. All 6 pairs are included; no outliers removed. These are preregistered robust pair statistics, not the failed two-process protocol.','scope':'Targeted R0 call attribution; not formal benchmark. QueryGuard checkSlow is only externally linked calls; same-object calls can bypass wrapper. Nested functions cannot be added.'}
(O/'protocol.json').write_text(json.dumps(protocol,indent=2)+'\n');pairs=[];observed=[];disabled_all=[];calls=[]
for i,order in enumerate(protocol['orders']):
 rows={}
 for name in order:
  stem=O/(str(i)+'-'+name);probe=C if name=='control' else P/'probe';cmd=['/usr/bin/taskset','-c','0',str(probe),'read',str(B/'medium.zyx'),f'/tmp/zyx-call-paired-{i}-{name}.db',protocol['case'],'4','16'];print('START',i,name,flush=True)
  with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:res=run(cmd,out,err,dict(campaign.ENV,ZYX_CPU_SAMPLE_ENABLE='1'),campaign.REG['limits'])
  stem.with_suffix('.resources.json').write_text(json.dumps(res,indent=2)+'\n');assert res['returncode']==0,res
  rows[name]=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(rows[name])==16
  for x in rows[name]:campaign.check_event(x['case'],x,'medium',False)
 a=[x for x in rows['instrumented'] if x['sampled']];b=[x for x in rows['instrumented'] if not x['sampled']];c=[x for x in rows['control'] if not x['sampled']];assert len(a)==len(b)==len(c)==8
 lines=(O/(str(i)+'-instrumented.stderr')).read_text().splitlines();indices={r['call_ordinal']:r for l in lines if l.startswith('{"call_ordinal"') for r in [json.loads(l)]};cal=[json.loads(l)['tsc_calibration_ratio'] for l in lines if l.startswith('{"tsc_calibration_ratio"')];assert len(cal)==1
 calls.extend((x,indices[x['iteration']+5]) for x in a);observed.extend(a);disabled_all.extend(b)
 ratio=statistics.median(x['latency_ms'] for x in b)/statistics.median(x['latency_ms'] for x in c)
 pairs.append({'pair':i,'disabled_control_ratio':ratio,'calibration_ratio':cal[0]});print('PAIR',i,round(ratio,5),flush=True)
median_ratio=statistics.median(x['disabled_control_ratio'] for x in pairs);mad=statistics.median(abs(x['disabled_control_ratio']-median_ratio) for x in pairs)
onoff=statistics.median(x['latency_ms'] for x in observed)/statistics.median(x['latency_ms'] for x in disabled_all);cpu=statistics.median((x['user_ms']+x['system_ms'])/x['latency_ms'] for x in observed)
costs=[]
for j,name in enumerate(['fstat','crc32','query_guard_checkSlow_external_only']):
 samples=sum(r['items'][j][2] for _,r in calls); inside_samples=sum(r['items'][j][3] for _,r in calls)
 fractions=[];inside_fractions=[]
 for x,r in calls:
  total,inside,n,ni,ns,nsi=r['items'][j]; wall=x['latency_ms']*1e6
  if n:fractions.append(ns/n*total/wall)
  if ni:inside_fractions.append(nsi/ni*inside/wall)
 costs.append({'function':name,'samples':samples,'inside_samples':inside_samples,'total_calls_per_query':statistics.median(r['items'][j][0] for _,r in calls),'inside_source_calls_per_query':statistics.median(r['items'][j][1] for _,r in calls),'median_query_elapsed_fraction':statistics.median(fractions) if fractions else None,'inside_source_elapsed_fraction':statistics.median(inside_fractions) if inside_fractions else None})
quality=abs(median_ratio-1)<=.05 and mad<=.05 and abs(onoff-1)<=.05 and abs(cpu-1)<=.05 and all(abs(x['calibration_ratio']-1)<=.01 for x in pairs) and all(x['samples']>=100 for x in costs)
assert sha(B/'medium.zyx')==protocol['source_sha256'] and sha(R/'build-scan-candidate/libzyx_core.a')==protocol['core_sha256']
result={'quality_passed':quality,'oracles':192,'pairwise_ratios':pairs,'median_pair_ratio':median_ratio,'pair_ratio_mad':mad,'onoff_ratio':onoff,'cpu_wall_ratio':cpu,'costs':costs,'interpretation':'Estimates allowed only if the preregistered quality rule passes. Counts of guard calls are incomplete due to static same-object binding. No exact CPU shares; costs overlap.'}
(O/'summary.json').write_text(json.dumps(result,indent=2)+'\n');print('COMPLETE',json.dumps(result),flush=True)
