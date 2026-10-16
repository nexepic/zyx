from pathlib import Path
import sys,os,time,json,argparse
sys.path.insert(0,'/tmp')
import importlib.util
spec=importlib.util.spec_from_file_location('final_window','/tmp/zyx-final-window.py');window=importlib.util.module_from_spec(spec);spec.loader.exec_module(window)
p=argparse.ArgumentParser();p.add_argument('--engine',required=True,choices=['control','current','kuzu']);p.add_argument('--round',type=int,required=True);p.add_argument('--seed',type=int,default=42);a=p.parse_args();window.restrict()
records=[]
if a.engine=='kuzu':
 import kuzu
 original_limits=window.kuzu_limits
 def traced_limits():
  original_limits(); original_database=kuzu.Database
  class Database(original_database):
   def __init__(self,*args,**kwargs):
    self._profile_closed=False;start=time.perf_counter_ns()
    try:super().__init__(*args,**kwargs)
    finally:records.append({'command':'DB_OPEN','query':'','ns':time.perf_counter_ns()-start})
   def close(self):
    if self._profile_closed:return super().close()
    self._profile_closed=True;start=time.perf_counter_ns()
    try:return super().close()
    finally:records.append({'command':'DB_CLOSE','query':'','ns':time.perf_counter_ns()-start})
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
     self.finished=True;records.append({'command':'QUERY','query':self.query[:240],'ns':time.perf_counter_ns()-self.start})
 class Connection:
  def __init__(self,*args,**kwargs):self.inner=original(*args,**kwargs)
  def __getattr__(self,name):return getattr(self.inner,name)
  def execute(self,query,*args,**kwargs):
   start=time.perf_counter_ns()
   try:result=self.inner.execute(query,*args,**kwargs)
   except Exception:
    records.append({'command':'QUERY','query':query[:240],'ns':time.perf_counter_ns()-start,'failed':True});raise
   if isinstance(result,list):return result
   return Cursor(result,query,start)
 kuzu.Connection=Connection
location=window.OUT/'contract'/f'round-{a.round}'/a.engine
begin=time.monotonic()
try:window.preflight(a.engine,'contract',location,timed=True,seed=a.seed)
finally:
 location.mkdir(parents=True,exist_ok=True)
 if a.engine=='kuzu':
  (location/'ipc-operation-timings.json').write_text(json.dumps({'scope':'Kuzu Python execute through full result drain, semantic validation and close; compare API/IPC overhead disclosed','operations':records},indent=2)+'\n')
 (location/'elapsed.json').write_text(json.dumps({'wall_seconds':time.monotonic()-begin,'maxrss_kib':__import__('resource').getrusage(__import__('resource').RUSAGE_SELF).ru_maxrss,'seed':a.seed},indent=2)+'\n')
