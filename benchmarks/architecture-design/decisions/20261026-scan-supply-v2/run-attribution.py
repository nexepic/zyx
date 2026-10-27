from pathlib import Path
import json,os,sys,subprocess,statistics,hashlib,time
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
source=campaign.INPUT_ROOT/'medium.scan-corrected-v1.zyx'
def state():
 return {'time_ns':time.time_ns(),'cpu_stat':Path('/sys/fs/cgroup/cpu.stat').read_text(),'loadavg':Path('/proc/loadavg').read_text()}
identity={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip()}
for name,p in [('input',source),('probe',D/'probe'),('library',R/'build-scan-candidate/libzyx_core.a')]:
 with p.open('rb') as f:identity[name]=hashlib.file_digest(f,'sha256').hexdigest()
(D/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
for case in ['all_nodes_property_filter','label_scan_filter']:
 stem=D/case;target=Path('/tmp')/('zyx-v2-isolated-'+case+'.zyx');assert not target.exists()
 cmd=[str(D/'probe'),'read',str(source),str(target),case,'4','40'];env=dict(os.environ,ZYX_CPU_SAMPLE_ENABLE='1',ZYX_CPU_SAMPLE_OUTPUT=str(stem.with_suffix('.pcs')))
 before=state();print('START',case,flush=True)
 with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:completed=subprocess.run(cmd,stdout=out,stderr=err,env=env,timeout=1200,preexec_fn=campaign.limits)
 after=state();events=[json.loads(x) for x in stem.with_suffix('.jsonl').read_text().splitlines()];assert completed.returncode==0 and len(events)==40
 for e in events:campaign.check_event(case,e,'medium',False)
 control=[e['latency_ms'] for e in events if not e['sampled']];sampled=[e['latency_ms'] for e in events if e['sampled']];ratio=statistics.median(sampled)/statistics.median(control)
 receipt={'command':cmd,'before':before,'after':after,'oracle':'passed','sampled_median':statistics.median(sampled),'control_median':statistics.median(control),'ratio':ratio,'within_five_percent':abs(ratio-1)<=.05,'formal_performance':False,'events':len(events)}
 stem.with_suffix('.receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('DONE',case,ratio,flush=True)
