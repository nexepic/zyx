from pathlib import Path
import subprocess,json,time
root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v3')
while not (root/'attribution/full-process-cpu/report.json').exists():time.sleep(1)
for phase,script in [('mechanisms','/tmp/run-zyx-final-mechanism-campaign.py'),('contracts','/tmp/run-zyx-final-contract-campaign.py'),('steady-state-report','/tmp/report-zyx-final-steady-state.py')]:
 print(json.dumps({'phase':phase,'state':'start'}),flush=True)
 with (root/'logs'/('final-'+phase+'.log')).open('w') as out:subprocess.run(['/workspace/zyx-benchmark-venv/bin/python',script],stdout=out,stderr=subprocess.STDOUT,check=True)
 print(json.dumps({'phase':phase,'state':'completed'}),flush=True)
(root/'campaign-completed.json').write_text(json.dumps({'status':'registered_phases_attempted_with_retained_failures_and_collector_limits','acceptance':'Requires per-stage review; no overall performance pass implied.'},indent=2)+'\n')
