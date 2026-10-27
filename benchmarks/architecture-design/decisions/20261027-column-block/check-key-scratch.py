"""Complete-result and allocation-count comparison only; no timing claims."""
from pathlib import Path
import subprocess,json,hashlib,sys,re
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;O=N/'key-scratch';D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort'
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
assert not (O/'work-receipt.json').exists()
link=json.loads((N/'current-cpu/command-1.json').read_text());old=N/'current-cpu'
link=[x.replace(str(old/'probe.o'),str(O/'allocation.o')).replace(str(old/'probe'),str(O/'allocation-probe')) for x in link]
link.insert(link.index(str(R/'build-scan-candidate/libzyx_core.a')),str(O/'runtime.o'))
(O/'allocation-link.json').write_text(json.dumps(link,indent=2)+'\n')
with (O/'allocation-link.log').open('w') as f:r=subprocess.run(link,cwd=R,stdout=f,stderr=subprocess.STDOUT)
if r.returncode:raise RuntimeError((O/'allocation-link.log').read_text()[-5000:])
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
source=B/'medium.zyx';core=R/'build-scan-candidate/libzyx_core.a';before=sha(source);core_before=sha(core);results=[]
for name,probe in [('control',B/'probes/allocation'),('candidate',O/'allocation-probe')]:
 stem=O/('allocation-'+name);cmd=[str(probe),'check-read',str(source),f'/tmp/zyx-key-scratch-{name}.db','multi_key_group','2','1'];print('START',name,flush=True)
 with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:res=run(cmd,out,err,dict(campaign.ENV,LD_PRELOAD=str(D/'allocation-observer.so'),ZYX_ALLOCATION_OUTPUT=str(stem.with_suffix('.stacks'))),campaign.REG['limits'])
 assert res['returncode']==0,res
 events=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(events)==1;campaign.check_event('multi_key_group',events[0],'medium',False)
 headers=[l for l in stem.with_suffix('.stacks').read_text().splitlines() if l.startswith('# window')];assert len(headers)==3
 m=re.search(r'allocations (\d+) requested_bytes (\d+) frees (\d+) failed (\d+)',headers[-1]);assert m
 results.append({'name':name,'complete_oracle':'pass','allocations':int(m[1]),'requested_bytes':int(m[2]),'frees':int(m[3]),'failed':int(m[4]),'probe_sha256':sha(probe),'resources':res});print(name,headers[-1],flush=True)
assert sha(source)==before and sha(core)==core_before
result={'scope':'Untimed original vs key-scratch experiment; no speedup claim; complete multi-key result compared to frozen oracle','source_sha256':before,'core_sha256':core_before,'runtime_sha256':sha(O/'runtime.o'),'results':results,'allocations_removed':results[0]['allocations']-results[1]['allocations'],'requested_bytes_removed':results[0]['requested_bytes']-results[1]['requested_bytes']}
(O/'work-receipt.json').write_text(json.dumps(result,indent=2)+'\n');print('COMPLETE',result['allocations_removed'],flush=True)
