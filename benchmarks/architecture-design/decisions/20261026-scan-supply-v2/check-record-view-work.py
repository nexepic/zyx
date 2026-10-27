"""Untimed medium work/allocation checks after the matching lifecycle oracle."""
from pathlib import Path
import hashlib, json, os, subprocess, sys, time
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent;B=D/'record-view-probes';O=D/'record-view-work'
deadline=time.monotonic()+2000
while not (B/'generality-receipt.json').exists():
    assert time.monotonic()<deadline
    time.sleep(1)
assert json.loads((B/'generality-receipt.json').read_text())['returncode']==0
assert not O.exists();O.mkdir()
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
source=campaign.INPUT_ROOT/'medium.scan-corrected-v1.zyx'
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
before=sha(source)
cases=['all_nodes_property_filter','label_scan_filter','aggregation_count_by_group','multi_key_group']
cmd=[str(B/'values'),'check-read',str(source),str(O/'work.db'),','.join(cases),'0','1']
with (O/'work.jsonl').open('w') as out,(O/'work.stderr').open('w') as err:
    p=subprocess.run(cmd,stdout=out,stderr=err,env=campaign.ENV,timeout=1200,preexec_fn=campaign.limits)
assert p.returncode==0
rows=[json.loads(x) for x in (O/'work.jsonl').read_text().splitlines()]
assert len(rows)==4
for case,event in zip(cases,rows):campaign.check_event(case,event,'medium',False)
(O/'work-receipt.json').write_text(json.dumps(dict(command=cmd,complete_oracles=4,timing_benchmark=False,binary_sha256=sha(B/'values')),indent=2)+'\n')
cmd=[str(B/'allocation'),'check-read',str(source),str(O/'allocation.db'),'all_nodes_property_filter','2','1']
env=dict(campaign.ENV,LD_PRELOAD=str(D/'allocation-observer.so'),ZYX_ALLOCATION_OUTPUT=str(O/'allocation.stacks'))
with (O/'allocation.jsonl').open('w') as out,(O/'allocation.stderr').open('w') as err:
    p=subprocess.run(cmd,stdout=out,stderr=err,env=env,timeout=1200,preexec_fn=campaign.limits)
assert p.returncode==0
rows=[json.loads(x) for x in (O/'allocation.jsonl').read_text().splitlines()];assert len(rows)==1
campaign.check_event('all_nodes_property_filter',rows[0],'medium',False)
header=[x for x in (O/'allocation.stacks').read_text().splitlines() if x.startswith('# window ')][-1]
allocations=int(header.split()[4]);assert sha(source)==before
(O/'allocation-receipt.json').write_text(json.dumps(dict(command=cmd,complete_oracle='passed',latency_usable=False,last_window=header,heap_allocations=allocations,target=36000,work_gate_passed=allocations<=36000,source_sha256=before,binary_sha256=sha(B/'allocation')),indent=2)+'\n')
print('COMPLETE four work oracles and allocation oracle;',allocations,'allocations; target <=36000',flush=True)
