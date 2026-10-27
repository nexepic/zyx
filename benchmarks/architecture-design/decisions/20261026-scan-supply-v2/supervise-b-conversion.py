"""Extend only offline supervision without restarting immutable workers.
The original Python wait(timeout) controllers are paused while their fresh
resource launchers continue. Once a launcher has reaped its worker and exited,
resume its original controller; waitpid observes completion before timeout.
The original controller still emits the complete wall time and launcher usage.
No candidate memory, file, query or benchmark-process limit is changed.
"""
from pathlib import Path
import json,os,signal,subprocess,time
D=Path(__file__).resolve().parent;B=D/'b-field-cohort'
pairs={'medium':(50248,50249,50250),'holdout':(50490,50491,50492)}
paused=set();checkpoint=B/'offline-supervision-adjustment.json'
def ps(pid,field):
 return subprocess.check_output(['ps','-p',str(pid),'-o',field+'='],text=True).strip()
def save(v):checkpoint.write_text(json.dumps(v,indent=2)+'\n')
record=dict(purpose='Correct offline-only watchdog shorter than known construction duration; preserve existing work and original resource receipts',old_offline_timeout_seconds=7200,new_offline_timeout_seconds=21600,query_benchmark_process_timeout_seconds=1200,default_query_timeout_ms=30000,candidate_workers_unchanged=True,resource_launchers_unchanged=True,reference='20261025-real-scan-admission/medium-resources.json:12413.1919 seconds',workers={})
started=time.monotonic()
for name,(controller,launcher,worker) in pairs.items():
 assert ps(launcher,'ppid')==str(controller)
 assert ps(worker,'ppid')==str(launcher)
 command=ps(worker,'args');assert 'zyx-scan-convert' in command and str(B/(name+'.zyx')) in command
 assert 'python3' in ps(controller,'args')
 assert 'resource-launcher' in ps(launcher,'args')
 age=int(ps(worker,'etimes'))
 record['workers'][name]=dict(controller_pid=controller,launcher_pid=launcher,worker_pid=worker,elapsed_at_transfer_seconds=age,deadline_monotonic=started+21600-age,status='validated_before_transfer')
save(record)
def restore(*_):
 for pid in tuple(paused):
  try:os.kill(pid,signal.SIGCONT)
  except ProcessLookupError:pass
  paused.discard(pid)
for sig in [signal.SIGTERM,signal.SIGINT]:
 def interrupt(signum,frame):restore();raise SystemExit(128+signum)
 signal.signal(sig,interrupt)
try:
 for name,(controller,launcher,worker) in pairs.items():
  os.kill(controller,signal.SIGSTOP);paused.add(controller)
  record['workers'][name]['status']='original_wait_controller_paused_worker_running'
 save(record);print('Offline supervision transferred; original worker/resource processes unchanged',flush=True)
 active=set(pairs)
 while active:
  for name in tuple(active):
   controller,launcher,worker=pairs[name];entry=record['workers'][name]
   try:state=ps(launcher,'stat')
   except subprocess.CalledProcessError:state='gone'
   if state.startswith('Z') or state=='gone':
    os.kill(controller,signal.SIGCONT);paused.discard(controller)
    entry['status']='launcher_completed_original_controller_resumed';active.remove(name);save(record)
    print(name,'launcher complete; original receipt collection resumed',flush=True)
   elif time.monotonic()>entry['deadline_monotonic']:
    os.killpg(launcher,signal.SIGKILL);os.kill(controller,signal.SIGCONT);paused.discard(controller)
    entry['status']='offline_six_hour_limit_exceeded_worker_group_terminated';active.remove(name);save(record)
    print(name,'offline limit exceeded; original controller resumed to preserve failure',flush=True)
  if active:time.sleep(1)
finally:restore()
