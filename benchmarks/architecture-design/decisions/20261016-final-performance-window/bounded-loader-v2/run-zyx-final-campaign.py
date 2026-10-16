from pathlib import Path
import subprocess,json,hashlib
root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v2')
python='/workspace/zyx-benchmark-venv/bin/python'
proof=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation/current-recovery-final-smoke/report.json')
report=json.loads(proof.read_text());assert report['full_results_checked'] and len(report['families'])==13
assert report['profile']=='smoke'  # Full-size family failures remain independently recorded, never implied passes.
assert report['binary_sha256']==hashlib.sha256(Path('/workspace/zyx/buildDir_perf_control/zyx-workload-contract-worker').read_bytes()).hexdigest()
steps=[('register',['/tmp/zyx-final-window.py','register']),('reads',['/tmp/run-zyx-final-read-campaign.py']),('read-report',['/tmp/report-zyx-final-reads.py']),('contracts',['/tmp/run-zyx-final-contract-campaign.py']),('steady-state-report',['/tmp/report-zyx-final-steady-state.py']),('attribution',['/tmp/zyx-final-profile.py']),('mechanisms',['/tmp/run-zyx-final-mechanism-campaign.py'])]
for phase,args in steps:
 print(json.dumps({'phase':phase,'state':'start'}),flush=True)
 if phase=='register':subprocess.run([python,*args],check=True)
 else:
  (root/'logs').mkdir(exist_ok=True)
  with (root/'logs'/f'campaign-{phase}.log').open('w') as output:
   subprocess.run([python,*args],stdout=output,stderr=subprocess.STDOUT,check=True)
 print(json.dumps({'phase':phase,'state':'completed'}),flush=True)
(root/'campaign-completed.json').write_text(json.dumps({'status':'all_registered_campaign_phases_completed','acceptance':'reports still require correctness, comparability, statistical and protection-gate review'})+'\n')
