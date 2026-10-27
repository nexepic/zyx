"""Medium-only boundary attribution with an unchanged production core."""
from pathlib import Path
import json,statistics,sys,hashlib
from process_receipt import run
D=Path(__file__).resolve().parent;R=Path('/workspace/zyx');O=D/'boundary-observer-tsc'
assert json.loads((D/'batch-handoff-compatibility-receipt.json').read_text())
assert json.loads((D/'batch-handoff-probes/generality-receipt.json').read_text())['returncode']==0
assert json.loads((D/'batch-handoff-work/batch-work-receipt.json').read_text())['all_source_rows_use_direct_borrowed_id_supply']
assert not (O/'events.jsonl').exists()
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
cmd=['/usr/bin/taskset','-c','0',str(O/'probe'),'read',str(campaign.INPUT_ROOT/'medium.scan-corrected-v1.zyx'),'/tmp/zyx-medium-boundary-observer-tsc.db','all_nodes_property_filter','4','80']
with (O/'events.jsonl').open('w') as out,(O/'stderr').open('w') as err:
 resources=run(cmd,out,err,dict(campaign.ENV,ZYX_CPU_SAMPLE_ENABLE='1'),campaign.REG['limits'])
(O/'resources.json').write_text(json.dumps(resources,indent=2)+'\n')
rows=[json.loads(s) for s in (O/'events.jsonl').read_text().splitlines()];assert resources['returncode']==0 and len(rows)==80
for row in rows:campaign.check_event(row['case'],row,'medium',False)
sampled=[x for x in rows if x['sampled']];control=[x for x in rows if not x['sampled']]
assert len(sampled)==len(control)==40
assert all(x['source_calls']>0 and x['callback_calls']>0 and 0<=x['callback_ns']<=x['source_ns'] for x in sampled)
ratio=statistics.median(x['latency_ms'] for x in sampled)/statistics.median(x['latency_ms'] for x in control)
def med(field):return statistics.median(x[field] for x in sampled)
shares=[(x['source_ns']-x['callback_ns'])/(x['latency_ms']*1e6) for x in sampled]
cpu_wall=statistics.median((x['user_ms']+x['system_ms'])/x['latency_ms'] for x in sampled)
calibration=[json.loads(x)['tsc_calibration_ratio'] for x in (O/'stderr').read_text().splitlines() if x.startswith('{\"tsc_calibration_ratio\"')];assert len(calibration)==1
calibration_ok=abs(calibration[0]-1)<=.01
assert all(0<=x<=1.01 for x in shares)
receipt=dict(command=cmd,formal_performance=False,complete_oracles=80,sampled_control_median_ratio=ratio,quality_passed=abs(ratio-1)<=.05 and abs(cpu_wall-1)<=.05 and calibration_ok,phase_basis='elapsed time from calibrated invariant TSC',cpu_wall_median_ratio=cpu_wall,tsc_calibration_audit_ratio=calibration[0],source_inclusive_median_ms=med('source_ns')/1e6,callback_median_ms=med('callback_ns')/1e6,source_exclusive_elapsed_median_share=statistics.median(shares),source_calls_median=med('source_calls'),callback_calls_median=med('callback_calls'),query_cpu_median_ms=statistics.median(x['user_ms']+x['system_ms'] for x in sampled),library_sha256=hashlib.file_digest((R/'build-scan-candidate/libzyx_core.a').open('rb'),'sha256').hexdigest(),interpretation='Elapsed attribution eligible only if quality_passed; not exact CPU shares; boundary excludes higher-level adapter and membership supply')
(O/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
