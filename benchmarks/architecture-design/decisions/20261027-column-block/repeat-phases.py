"""Repeat the previously rejected internal-phase observer with two preregistered control-bracketed processes."""
from pathlib import Path
import json,sys,statistics,hashlib
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;O=N/'phase-repeat';O.mkdir(exist_ok=False);P=N/'current-phases';D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort'
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
labels=['source remainder','group lookup/fence','column binding','field frame acquisition/cache','record decode/validation','membership cache/validation','typed group preparation','consumer/admission callbacks']
protocol={'cycles':2,'order':['control-before','phases','control-after'],'warmups':4,'iterations':64,'scope':'Targeted medium all-node diagnostic, not an acceptance benchmark. Both independent cycles must pass all pre-existing quality gates; failures remain excluded.','quality_max_fraction':.05,'cpu_wall_max_fraction':.05,'calibration_max_fraction':.01,'max_phase_fraction_difference':.10,'source_sha256':sha(B/'medium.zyx'),'core_sha256':sha(R/'build-scan-candidate/libzyx_core.a'),'phase_probe_sha256':sha(P/'probe'),'control_probe_sha256':sha(B/'probes/public')}
(O/'protocol.json').write_text(json.dumps(protocol,indent=2)+'\n');results=[]
for cycle in range(2):
 rows={}
 for name in protocol['order']:
  probe=P/'probe' if name=='phases' else B/'probes/public';stem=O/(str(cycle)+'-'+name);cmd=['/usr/bin/taskset','-c','0',str(probe),'read',str(B/'medium.zyx'),f'/tmp/zyx-phase-repeat-{cycle}-{name}.db','all_nodes_property_filter','4','64'];print('START',cycle,name,flush=True)
  with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:res=run(cmd,out,err,dict(campaign.ENV,ZYX_CPU_SAMPLE_ENABLE='1'),campaign.REG['limits'])
  stem.with_suffix('.resources.json').write_text(json.dumps(res,indent=2)+'\n');assert res['returncode']==0,res
  rows[name]=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(rows[name])==64
  for x in rows[name]:campaign.check_event(x['case'],x,'medium',False)
 a=[x for x in rows['phases'] if x['sampled']];b=[x for x in rows['phases'] if not x['sampled']];assert len(a)==len(b)==32
 med=lambda xs,key:statistics.median(x[key] for x in xs)
 enabled=med(a,'latency_ms')/med(b,'latency_ms');external=med(b,'latency_ms')/med(rows['control-before']+rows['control-after'],'latency_ms');cpu=statistics.median((x['user_ms']+x['system_ms'])/x['latency_ms'] for x in a)
 logs=(O/(str(cycle)+'-phases.stderr')).read_text().splitlines();phases={x['phase_ordinal']:x for l in logs if l.startswith('{"phase_ordinal"') for x in [json.loads(l)]};cal=[json.loads(l)['tsc_calibration_ratio'] for l in logs if l.startswith('{"tsc_calibration_ratio"')];assert len(cal)==1
 selected=[phases[x['iteration']+5] for x in a];assert sum(x['counts'][0] for x in selected)==440
 ns=[sum(x['ns'][i] for x in selected) for i in range(8)];share=statistics.median((x['source_ns']-x['callback_ns'])/(x['latency_ms']*1e6) for x in a)
 quality=abs(enabled-1)<=.05 and abs(external-1)<=.05 and abs(cpu-1)<=.05 and abs(cal[0]-1)<=.01
 record={'cycle':cycle,'oracles':192,'quality_passed':quality,'enabled_disabled_ratio':enabled,'disabled_unmodified_ratio':external,'cpu_wall_ratio':cpu,'calibration_ratio':cal[0],'exclusive_supply_share':share,'phase_ns':ns,'phase_estimates':[{'phase':label,'elapsed_share':ns[i]/sum(ns[:7])*share if i<7 else statistics.median(x['callback_ns']/(x['latency_ms']*1e6) for x in a)} for i,label in enumerate(labels)]};results.append(record);print('CYCLE',json.dumps(record),flush=True)
passed=all(x['quality_passed'] for x in results) and all(abs(a['elapsed_share']-b['elapsed_share'])<=.10 for a,b in zip(results[0]['phase_estimates'],results[1]['phase_estimates']))
assert sha(B/'medium.zyx')==protocol['source_sha256'] and sha(R/'build-scan-candidate/libzyx_core.a')==protocol['core_sha256']
(O/'summary.json').write_text(json.dumps({'quality_passed':passed,'cycles':results},indent=2)+'\n');print('COMPLETE',passed,flush=True)
