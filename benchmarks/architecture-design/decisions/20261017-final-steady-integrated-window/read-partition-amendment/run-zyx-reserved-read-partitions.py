from pathlib import Path
import json,time,subprocess,os,signal,sys,importlib
sys.path.insert(0,'/tmp');owned=importlib.import_module('zyx-final-five-owned-case')
r=Path('/tmp/zyx-final-five-steady-window-20261017');a=json.loads((r/'read-partition-amendment.json').read_text())
while not (r/'read-partition-boundary.json').exists():time.sleep(1)
assert (r/'bounded-read-extensions-completed.json').exists()
assert not (r/'read-partition-resolved.json').exists()
start=time.monotonic();results=[]
for i,names in enumerate(a['partitions'],1):
 remaining=a['remaining_control_profile_budget_seconds']-(time.monotonic()-start)
 if remaining<=0:
  results.append({'partition':i,'workloads':names,'status':'inconclusive_global_budget_exhausted'});continue
 log=r/'logs'/f'reserved-control-scan-round5-partition{i}.log'
 env=dict(os.environ,ZYX_FINAL_EXTENDED_WORKLOADS=json.dumps(names))
 with log.open('w') as out:
  p=subprocess.Popen([sys.executable,'/tmp/zyx-final-steady-extended-read.py','--engine','control','--round','5','--profile','scan','--scale','medium'],env=env,stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
  timed=False
  try:p.wait(timeout=min(1800,remaining))
  except subprocess.TimeoutExpired:timed=True;os.killpg(p.pid,signal.SIGKILL);p.wait()
 owned.cleanup_read(r,'medium',5,'control','scan')
 entry={'partition':i,'workloads':names,'status':'inconclusive_timeout' if timed else ('completed' if p.returncode==0 else 'failed'),'returncode':p.returncode,'log':str(log),'elapsed_global_seconds':time.monotonic()-start};results.append(entry)
 (r/f'reserved-control-partition-{i}-status.json').write_text(json.dumps(entry,indent=2)+'\n');print(json.dumps(entry),flush=True)
(r/'read-partition-resolved.json').write_text(json.dumps({'partitions':results,'original_cancelled_attempt_still_failed':True,'no_more_attempts_allowed':True,'elapsed_seconds':time.monotonic()-start,'budget_seconds':a['remaining_control_profile_budget_seconds']},indent=2)+'\n')
