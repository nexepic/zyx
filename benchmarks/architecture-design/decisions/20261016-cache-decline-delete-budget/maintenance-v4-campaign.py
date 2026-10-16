from pathlib import Path
import subprocess,json,os,signal
root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-maintenance-v4')
registration=json.loads((root/'registration.json').read_text())
assert registration['status']=='registered_before_measurement'
env=os.environ.copy();env.update(ZYX_CONTRACT_OUTPUT_ROOT=str(root),ZYX_FROZEN_BASELINE_WORKER=registration['current_frozen_worker'],ZYX_FROZEN_BASELINE_HARNESS=registration['frozen_harness'])
results=[]
for number,order in enumerate(registration['orders'],1):
 for engine in order:
  case=root/'contract'/f'round-{number}'/engine/'maintenance'
  marker=next((case/n for n in ['campaign-case-completed.json','campaign-case-failed.json'] if (case/n).exists()),None)
  if marker:results.append(json.loads(marker.read_text()));continue
  previous=next((v for v in results if v['engine']==engine and v['status']!='completed'),None)
  if previous:
   case.mkdir(parents=True,exist_ok=True);entry={'round':number,'engine':engine,'family':'maintenance','status':'unmeasurable_previous_failure','source':previous,'rule':'Do not repeat unchanged failure; no accepted latency sample or protection pass.'}
   (case/'campaign-case-failed.json').write_text(json.dumps(entry,indent=2)+'\n');results.append(entry);continue
  log=root/'logs'/f'contract-{number}-{engine}-maintenance.log';print(json.dumps({'event':'case-start','round':number,'engine':engine}),flush=True)
  timeout=False
  with log.open('w') as stream:
   process=subprocess.Popen(['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-contract-case.py','--engine',engine,'--round',str(number),'--seed','42','--family','maintenance'],stdout=stream,stderr=subprocess.STDOUT,start_new_session=True,env=env)
   try:process.wait(timeout=1800)
   except subprocess.TimeoutExpired:timeout=True;os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
   except BaseException:
    if process.poll() is None:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
    raise
  case.mkdir(parents=True,exist_ok=True);entry={'round':number,'engine':engine,'family':'maintenance','status':'inconclusive_timeout' if timeout else ('completed' if process.returncode==0 else 'failed'),'returncode':process.returncode,'log':str(log)}
  (case/('campaign-case-completed.json' if entry['status']=='completed' else 'campaign-case-failed.json')).write_text(json.dumps(entry,indent=2)+'\n');results.append(entry);print(json.dumps(entry),flush=True)
(root/'contract-campaign-completed.json').write_text(json.dumps({'cases':results,'rounds':3,'status':'all_registered_maintenance_cases_attempted'},indent=2)+'\n')
