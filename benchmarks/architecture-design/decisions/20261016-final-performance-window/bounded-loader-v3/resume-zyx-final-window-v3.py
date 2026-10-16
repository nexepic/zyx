from pathlib import Path
import subprocess,json,time,sys,os
root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v3');python='/workspace/zyx-benchmark-venv/bin/python'
# The already running reader owns its current cases. Wait without launching
# any second timing/profiling process. Only repair the zero-sample probe case.
while not (root/'read-campaign-completed.json').exists():time.sleep(1)
steps=[('reads-corrected-setup-retry','/tmp/run-zyx-final-read-campaign.py'),('read-report','/tmp/report-zyx-final-reads.py'),('attribution','/tmp/zyx-final-profile.py'),('mechanisms','/tmp/run-zyx-final-mechanism-campaign.py'),('contracts','/tmp/run-zyx-final-contract-campaign.py'),('steady-state-report','/tmp/report-zyx-final-steady-state.py')]
(root/'remaining-phase-order.json').write_text(json.dumps({'order':[v[0] for v in steps],'reason':'Original dispatcher stopped on an independently retained profile failure. Preserve completed timing; retry only zero-sample Kuzu setup. Run separate attribution before family matrix so its report is not delayed by unrelated bounded failures. No profiling overlaps formal timings.','production_binary_changes':False},indent=2)+'\n')
for phase,script in steps:
 print(json.dumps({'phase':phase,'state':'start'}),flush=True)
 with (root/'logs'/('resumed-'+phase+'.log')).open('w') as stream:subprocess.run([python,script],stdout=stream,stderr=subprocess.STDOUT,check=True)
 print(json.dumps({'phase':phase,'state':'completed'}),flush=True)
(root/'campaign-completed.json').write_text(json.dumps({'status':'all_registered_campaign_phases_attempted','acceptance':'Success, failure, unsupported and inconclusive outcomes require explicit report review; not an overall performance pass.'},indent=2)+'\n')
