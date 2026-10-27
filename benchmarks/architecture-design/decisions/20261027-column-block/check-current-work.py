from pathlib import Path
import sys,json,hashlib
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort';O=N/'current-work';O.mkdir(exist_ok=False)
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
for case in ('all_nodes_property_filter','label_scan_filter','multi_key_group'):
 stem=O/case;cmd=[str(B/'probes/allocation'),'check-read',str(B/'medium.zyx'),'/tmp/zyx-current-allocation-'+case+'.db',case,'2','1']
 with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:r=run(cmd,out,err,dict(campaign.ENV,LD_PRELOAD=str(D/'allocation-observer.so'),ZYX_ALLOCATION_OUTPUT=str(stem.with_suffix('.stacks'))),campaign.REG['limits'])
 assert r['returncode']==0,r
 events=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(events)==1
 campaign.check_event(case,events[0],'medium',False)
 headers=[l for l in stem.with_suffix('.stacks').read_text().splitlines() if l.startswith('# window')]
 receipt=dict(command=cmd,resources=r,complete_oracle='passed',timing_usable=False,allocation_windows=headers,probe_sha256=hashlib.sha256((B/'probes/allocation').read_bytes()).hexdigest())
 stem.with_suffix('.receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(case,headers[-1],flush=True)
