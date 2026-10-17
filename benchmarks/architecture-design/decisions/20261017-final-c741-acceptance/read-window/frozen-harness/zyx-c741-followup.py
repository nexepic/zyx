from pathlib import Path
import os,time,json,subprocess,signal,sys,importlib
sys.path.insert(0,'/tmp');owned=importlib.import_module('zyx-final-five-owned-case')
r=Path('/tmp/zyx-final-five-c741-window-20261017')
while Path('/proc/166968').exists():time.sleep(15)
results=[]
def execute(args,key):
 marker=r/'followup-status'/(key+'.json');marker.parent.mkdir(exist_ok=True)
 if marker.exists():return json.loads(marker.read_text())
 log=r/'logs'/(key+'.log');print(json.dumps({'start':key}),flush=True)
 with log.open('w') as out:
  p=subprocess.Popen(['/workspace/zyx-benchmark-venv/bin/python',*args],stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
  try:p.wait(timeout=1800);status='completed' if p.returncode==0 else 'failed'
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait();status='inconclusive_timeout'
 result={'key':key,'status':status,'returncode':p.returncode,'log':str(log)};marker.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True);return result
for number,order in [(4,['control','current']),(5,['current','control']),(6,['control','current'])]:
 for engine in order:
  prior=next((v for v in results if v.get('engine')==engine and v.get('kind')=='write' and v['status']!='completed'),None)
  if prior:continue
  v=execute(['/tmp/zyx-final-c741-write-case.py','--engine',engine,'--round',str(number)],f'write-v2-{number}-{engine}');v.update(engine=engine,kind='write');results.append(v);owned.cleanup_contract(r,f'write-round-{number}-{engine}')
for scale,profile in [('medium','indexed'),('holdout','retrieval')]:
 for number,order in [(4,['control','current']),(5,['current','control'])]:
  for engine in order:
   v=execute(['/tmp/zyx-c741-extended-read.py','read','--engine',engine,'--scale',scale,'--profile',profile,'--round',str(number),'--iterations','100'],f'extended-{scale}-{profile}-{number}-{engine}');results.append(v);owned.cleanup_read(r,scale,number,engine,profile)
(r/'followup-completed.json').write_text(json.dumps(results,indent=2)+'\n')
