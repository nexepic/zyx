from pathlib import Path
import subprocess,json,signal,sys,importlib,hashlib
sys.path.insert(0,'/tmp');owned_case=importlib.import_module('zyx-final-five-owned-case')
root=Path('/tmp/zyx-final-five-steady-window-20261017');results=[]
# Engine-free boundary for the unused reserved control round5 profile slot.
if (root/'read-partition-amendment.json').exists():
 import time
 if not (root/'read-partition-resolved.json').exists():
  (root/'read-partition-boundary.json').write_text(json.dumps({'pid':__import__('os').getpid(),'parent_pid':__import__('os').getppid(),'engine_started':False,'reason':'Complete only the unused round5 control slot through preregistered-budget partitioning before writes.'},indent=2)+'\n')
  while not (root/'read-partition-resolved.json').exists():time.sleep(.2)

for number,order in enumerate([['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']],1):
 for engine in order:
  marker=root/'write-case-status'/f'round-{number}'/(engine+'.json');marker.parent.mkdir(parents=True,exist_ok=True)
  if marker.exists():results.append(json.loads(marker.read_text()));continue
  previous=next((v for v in results if v['engine']==engine and v['status']!='completed'),None)
  entry={'engine':engine,'round':number}
  if previous:entry.update(status='unmeasurable_previous_failure',source=previous)
  else:
   print(json.dumps({'event':'write-profile-start',**entry}),flush=True);log=root/'logs'/f'write-{number}-{engine}.log';timeout=False
   with log.open('w') as output:
    process=subprocess.Popen(['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-steady-write-case.py','--engine',engine,'--round',str(number)],stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
    try:process.wait(timeout=1800)
    except subprocess.TimeoutExpired:timeout=True;__import__('os').killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
    except BaseException:
     if process.poll() is None:__import__('os').killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
     raise
   owned_case.cleanup_contract(root,f'write-round-{number}-{engine}')
   entry.update(status='inconclusive_timeout' if timeout else ('completed' if process.returncode==0 else 'failed'),returncode=process.returncode,log=str(log))
  marker.write_text(json.dumps(entry,indent=2)+'\n');results.append(entry);print(json.dumps(entry),flush=True)
(root/'write-campaign-completed.json').write_text(json.dumps({'cases':results,'status':'all_registered_write_profiles_accounted_requires_gate_review'},indent=2)+'\n')
