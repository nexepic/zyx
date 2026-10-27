"""One medium-only residual-cost diagnostic after primary rejection."""
from pathlib import Path
import json,hashlib,os,statistics,sys
from process_receipt import run
D=Path(__file__).resolve().parent;R=Path('/workspace/zyx')
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'))
import campaign
out=D/'record-view-residual';assert not out.exists();out.mkdir()
cmd=['/usr/bin/taskset','-c','0',str(D/'record-view-residual-cpu'),'read',str(campaign.INPUT_ROOT/'medium.scan-corrected-v1.zyx'),'/tmp/zyx-record-view-residual.db','all_nodes_property_filter','4','80']
env=dict(campaign.ENV,ZYX_CPU_SAMPLE_ENABLE='1',ZYX_CPU_SAMPLE_OUTPUT=str(out/'samples.pcs'))
with (out/'events.jsonl').open('w') as stdout,(out/'stderr').open('w') as stderr:
 resources=run(cmd,stdout,stderr,env,campaign.REG['limits'])
(out/'resources.json').write_text(json.dumps(resources,indent=2)+'\n')
rows=[json.loads(x) for x in (out/'events.jsonl').read_text().splitlines()]
assert resources['returncode']==0 and len(rows)==80
for row in rows:campaign.check_event(row['case'],row,'medium',False)
medians={str(s):statistics.median(x['latency_ms'] for x in rows if x['sampled']==s) for s in [True,False]}
ratio=medians['True']/medians['False']
receipt=dict(command=cmd,formal_performance=False,complete_oracles=len(rows),sampled_median_ms=medians['True'],control_median_ms=medians['False'],ratio=ratio,within_five_percent=abs(ratio-1)<=.05,library_sha256=hashlib.file_digest((R/'build-scan-candidate/libzyx_core.a').open('rb'),'sha256').hexdigest())
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
