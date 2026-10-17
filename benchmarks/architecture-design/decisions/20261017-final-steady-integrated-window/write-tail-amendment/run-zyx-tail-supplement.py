from pathlib import Path
import json,time,subprocess,os,signal,sys,importlib
sys.path.insert(0,'/tmp');owned=importlib.import_module('zyx-final-five-owned-case');r=Path('/tmp/zyx-final-five-steady-window-20261017');a=json.loads((r/'write-tail-amendment.json').read_text())
while not (r/'write-tail-boundary.json').exists():time.sleep(1)
assert (r/'bounded-write-extensions-completed.json').exists()
used=(r/'bounded-write-extensions-completed.json').stat().st_mtime-(r/'bounded-write-extension-registration.json').stat().st_mtime
budget=max(0,7200-used);start=time.monotonic();results=[]
for number,order in [(4,['control','current']),(5,['current','control'])]:
 for engine in order:
  entry={'round':number,'engine':engine,'workloads':[x['workload'] for x in a['cases']]};remaining=budget-(time.monotonic()-start)
  if remaining<=0:entry['status']='inconclusive_global_budget_exhausted'
  else:
   log=r/'logs'/f'write-tail-{number}-{engine}.log';command=[sys.executable,'/tmp/zyx-final-steady-tail-write-case.py','--engine',engine,'--round',str(number),'--samples','100']
   for x in a['cases']:command.extend(['--workload',x['workload']])
   with log.open('w') as out:
    p=subprocess.Popen(command,stdout=out,stderr=subprocess.STDOUT,start_new_session=True);timed=False
    try:p.wait(timeout=min(1800,remaining))
    except subprocess.TimeoutExpired:timed=True;os.killpg(p.pid,signal.SIGKILL);p.wait()
   owned.cleanup_contract(r,f'write-tail-round-{number}-{engine}');entry.update(status='inconclusive_timeout' if timed else ('completed' if p.returncode==0 else 'failed'),returncode=p.returncode,log=str(log))
  results.append(entry);(r/f'write-tail-{number}-{engine}-status.json').write_text(json.dumps(entry,indent=2)+'\n');print(json.dumps(entry),flush=True)
(r/'write-tail-resolved.json').write_text(json.dumps({'cases':results,'original_budget_seconds':7200,'previous_six_workload_seconds':used,'tail_budget_seconds':budget,'tail_elapsed_seconds':time.monotonic()-start,'no_more_same_source_attempts_allowed':True},indent=2)+'\n')
