from pathlib import Path
import sys,os,time,json,argparse
sys.path.insert(0,'/tmp')
import importlib.util
spec=importlib.util.spec_from_file_location('final_window','/tmp/zyx-final-repaired-window.py');window=importlib.util.module_from_spec(spec);spec.loader.exec_module(window)
p=argparse.ArgumentParser();p.add_argument('--engine',required=True,choices=['control','current','kuzu']);p.add_argument('--round',type=int,required=True);p.add_argument('--seed',type=int,default=42);p.add_argument('--family',required=True);a=p.parse_args()
os.environ['ZYX_FINAL_CASE_FAMILY']=a.family
if a.family=='wide_large':
 decision_path=window.OUT/'wide-resource-decision.json'
 if not decision_path.exists():raise RuntimeError('Wide resource amendment awaits the user reply')
 decision=json.loads(decision_path.read_text())
 if decision.get('approved') is True:
  assert decision['file_limit_bytes']==4*1024**3
  os.environ['ZYX_FINAL_CASE_FILE_BYTES']=str(decision['file_limit_bytes'])
 else:os.environ['ZYX_FINAL_CASE_FILE_BYTES']=str(2*1024**3)
window.restrict()
if os.environ.get('ZYX_FROZEN_BASELINE_HARNESS'):
 frozen=Path(os.environ['ZYX_FROZEN_BASELINE_HARNESS'])
 assert window.digest(frozen)==json.loads((window.OUT/'registration.json').read_text())['preflight_runner_sha256']
 frozen_spec=importlib.util.spec_from_file_location('runner.workload_preflight',frozen)
 frozen_module=importlib.util.module_from_spec(frozen_spec);sys.modules['runner.workload_preflight']=frozen_module;frozen_spec.loader.exec_module(frozen_module)
 import runner;runner.workload_preflight=frozen_module
import runner.workload_preflight as preflight_module
import importlib
owned_case=importlib.import_module('zyx-final-five-owned-case')
owned_case.install_temp_journal(window.OUT,f'round-{a.round}-{a.engine}-{a.family}',preflight_module.tempfile)
def phase():return getattr(preflight_module._active_family_progress,"sampling_phase","setup")
records=[]
original_native_close=preflight_module.NativeWorker.close
def native_close_with_stats(self):
 try:
  if self.process.poll() is None:
   stats=self.call('STATS')
   target=window.OUT/'contract'/f'round-{a.round}'/a.engine/a.family/'final-storage-resources.json'
   target.write_text(json.dumps({'engine':a.engine,'scope':'end-of-family before worker close; counters include setup and all foreground/maintenance work, not query-only or device IO','stats':stats},indent=2)+'\n')
 except Exception as error:
  target=window.OUT/'contract'/f'round-{a.round}'/a.engine/a.family/'final-storage-resource-capture-error.json'
  target.parent.mkdir(parents=True,exist_ok=True);target.write_text(json.dumps({'capture_error':str(error),'not_a_successful_resource_observation':True},indent=2)+'\n')
 return original_native_close(self)
preflight_module.NativeWorker.close=native_close_with_stats
if a.engine=='kuzu':
 import kuzu
 original_limits=window.kuzu_limits
 def traced_limits():
  original_limits(); original_database=kuzu.Database
  class Database(original_database):
   def __init__(self,*args,**kwargs):
    self._observed_database=Path(args[0]);self._profile_closed=False;start=time.perf_counter_ns()
    try:super().__init__(*args,**kwargs)
    finally:records.append({'command':'DB_OPEN','query':'','sampling_phase':phase(),'ns':time.perf_counter_ns()-start})
   def close(self):
    if self._profile_closed:return super().close()
    self._profile_closed=True;start=time.perf_counter_ns()
    try:return super().close()
    finally:
     records.append({'command':'DB_CLOSE','query':'','sampling_phase':phase(),'ns':time.perf_counter_ns()-start})
     parent=self._observed_database.parent
     files=[p for p in parent.rglob('*') if p.is_file()]
     target=window.OUT/'contract'/f'round-{a.round}'/a.engine/a.family/'final-storage-resources.json'
     target.parent.mkdir(parents=True,exist_ok=True)
     target.write_text(json.dumps({'engine':a.engine,'scope':'end of most recent database close/checkpoint; all files in this private owned temporary database directory, including sidecars','file_bytes':sum(p.stat().st_size for p in files),'files':[str(p.relative_to(parent)) for p in files],'read_write_bytes':'unavailable; do not invent device/database IO from file size'},indent=2)+'\n')
  kuzu.Database=Database
 window.kuzu_limits=traced_limits
 original=kuzu.Connection
 class Cursor:
  def __init__(self,inner,query,start):self.inner=inner;self.query=query;self.start=start;self.finished=False
  def __getattr__(self,name):return getattr(self.inner,name)
  def close(self):
   try:return self.inner.close()
   finally:
    if not self.finished:
     self.finished=True;records.append({'command':'QUERY','query':self.query[:240],'sampling_phase':phase(),'ns':time.perf_counter_ns()-self.start})
 class Connection:
  def __init__(self,*args,**kwargs):self.inner=original(*args,**kwargs)
  def __getattr__(self,name):return getattr(self.inner,name)
  def execute(self,query,*args,**kwargs):
   start=time.perf_counter_ns()
   try:result=self.inner.execute(query,*args,**kwargs)
   except Exception:
    records.append({'command':'QUERY','query':query[:240],'sampling_phase':phase(),'ns':time.perf_counter_ns()-start,'failed':True});raise
   if isinstance(result,list):return result
   return Cursor(result,query,start)
 kuzu.Connection=Connection
location=window.OUT/'contract'/f'round-{a.round}'/a.engine/a.family
begin=time.monotonic()
try:window.preflight(a.engine,'contract',location,timed=True,seed=a.seed,family=a.family)
finally:
 location.mkdir(parents=True,exist_ok=True)
 if a.engine=='kuzu':
  (location/'ipc-operation-timings.json').write_text(json.dumps({'scope':'Kuzu Python execute through full result drain, semantic validation and close; compare API/IPC overhead disclosed','operations':records},indent=2)+'\n')
 (location/'elapsed.json').write_text(json.dumps({'wall_seconds':time.monotonic()-begin,'maxrss_kib':__import__('resource').getrusage(__import__('resource').RUSAGE_SELF if a.engine=='kuzu' else __import__('resource').RUSAGE_CHILDREN).ru_maxrss,'rss_scope':'Kuzu in-process driver and engine' if a.engine=='kuzu' else 'native worker child peak; one worker per isolated case','driver_maxrss_kib':__import__('resource').getrusage(__import__('resource').RUSAGE_SELF).ru_maxrss,'seed':a.seed},indent=2)+'\n')
