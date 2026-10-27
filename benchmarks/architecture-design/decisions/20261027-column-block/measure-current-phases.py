"""R0 sampled internal phases with unchanged-core controls before and after."""
from pathlib import Path
import json,sys,statistics,hashlib
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;O=N/'current-phases';D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort'
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
sha=lambda p:hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
labels=['source remainder','group lookup/fence','column binding','field frame acquisition/cache','record decode/validation','membership cache/validation','typed group preparation','consumer/admission callbacks']
assert not (O/'protocol.json').exists();protocol={'scope':'Medium all-node only; 1/32 source batches with rotating offset,32 measured observed queries cover all32 offsets','phases':labels,'iterations':64,'warmups':4,'quality_max_fraction':.05,'extra_quality_check':'pooled unmodified production public controls before/after versus observer-disabled samples','input_sha256':sha(B/'medium.zyx'),'core_sha256':sha(R/'build-scan-candidate/libzyx_core.a'),'runner_sha256':sha(__file__),'phases_sha256':sha(O/'phases.hpp')};(O/'protocol.json').write_text(json.dumps(protocol,indent=2)+'\n')
allrows={}
for name in ('control-before','phases','control-after'):
 probe=O/'probe' if name=='phases' else B/'probes/public';stem=O/name;cmd=['/usr/bin/taskset','-c','0',str(probe),'read',str(B/'medium.zyx'),'/tmp/zyx-current-phases-'+name+'.db','all_nodes_property_filter','4','64'];print('START',name,flush=True)
 with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:r=run(cmd,out,err,dict(campaign.ENV,ZYX_CPU_SAMPLE_ENABLE='1'),campaign.REG['limits'])
 stem.with_suffix('.resources.json').write_text(json.dumps(r,indent=2)+'\n');assert r['returncode']==0,r
 rows=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(rows)==64
 for x in rows:campaign.check_event(x['case'],x,'medium',False)
 allrows[name]=rows;print('PASS',name,flush=True)
a=[x for x in allrows['phases'] if x['sampled']];b=[x for x in allrows['phases'] if not x['sampled']];assert len(a)==len(b)==32
med=lambda rows,field:statistics.median(x[field] for x in rows)
ratio=med(a,'latency_ms')/med(b,'latency_ms');external=med(b,'latency_ms')/med(allrows['control-before']+allrows['control-after'],'latency_ms');cpu=statistics.median((x['user_ms']+x['system_ms'])/x['latency_ms'] for x in a)
logs=(O/'phases.stderr').read_text().splitlines();phases={x['phase_ordinal']:x for l in logs if l.startswith('{"phase_ordinal"') for x in [json.loads(l)]};cal=[json.loads(l)['tsc_calibration_ratio'] for l in logs if l.startswith('{"tsc_calibration_ratio"')];assert len(cal)==1
selected=[phases[x['iteration']+5] for x in a];assert sum(x['counts'][0] for x in selected)==440
ns=[sum(x['ns'][i] for x in selected) for i in range(8)];counts=[sum(x['counts'][i] for x in selected) for i in range(8)]
share=statistics.median((x['source_ns']-x['callback_ns'])/(x['latency_ms']*1e6) for x in a)
quality=abs(ratio-1)<=.05 and abs(external-1)<=.05 and abs(cpu-1)<=.05 and abs(cal[0]-1)<=.01
result={'oracles':192,'quality_passed':quality,'enabled_disabled_ratio':ratio,'disabled_unmodified_ratio':external,'cpu_wall_ratio':cpu,'calibration_ratio':cal[0],'source_exclusive_elapsed_share':share,'phase_sampled_ns':ns,'phase_counts':counts,'phase_estimates':[{'phase':label,'estimated_query_elapsed_share':ns[i]/sum(ns[:7])*share if i<7 else statistics.median(x['callback_ns']/(x['latency_ms']*1e6) for x in a)} for i,label in enumerate(labels)],'interpretation':'Estimates eligible only if all quality checks pass. Nested phases exclusive; outside-source costs are not assigned. Not exact CPU shares.'}
assert sha(B/'medium.zyx')==protocol['input_sha256'];assert sha(R/'build-scan-candidate/libzyx_core.a')==protocol['core_sha256']
(O/'receipt.json').write_text(json.dumps(result,indent=2)+'\n');print('COMPLETE',json.dumps(result),flush=True)
