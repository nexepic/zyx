from pathlib import Path
import importlib.util,subprocess,os,signal,json,time,threading,csv,shutil
s=importlib.util.spec_from_file_location('w','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(s);s.loader.exec_module(w);w.restrict()
from runner.semantic_oracle import verify_groups,verify_topk
root=w.OUT/'attribution'
if root.exists():
 old=w.OUT/'attribution-combined-collector-timeout';assert not old.exists();root.rename(old)
 cleanup=[]
 for p in old.rglob('private.zyx*'):
  if p.is_file():cleanup.append({'path':str(p),'bytes':p.stat().st_size});p.unlink()
 (old/'owned-clone-cleanup.json').write_text(json.dumps(cleanup,indent=2)+'\n')
root.mkdir(exist_ok=False)
data=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation/dataset')
with (data/'users.csv').open() as stream:users=list(csv.DictReader(stream))
registration={'scope':'Separate diagnostic processes only, never formal timing. CPU collection without heap tracing or native timers; heap collection only a bounded 200ms wall window, not whole-query allocation totals; native value counters in a separate process.','reason':'Combined unbounded heap tracing reached the existing 30000ms query timeout; preserve failed experiment. No deadline or memory increase.','cpu_iterations':{'scan_aggregate_sort':3,'point_traversal':100},'heap_iterations':3,'warmups':3,'heap_window_wall_seconds':0.2,'caveats':'Signals are approximate boundaries; a final cleanup race is recorded. Heap-window allocations include collector/entry effects and cannot be extrapolated to whole queries. CPU samples and counter processes are distinct. Process I/O may be unavailable and is not physical database I/O.','binary_sha256':w.digest(w.CURRENT/'zyx-compare-bench'),'formal_timing_samples_accepted':0}
(root/'registration.json').write_text(json.dumps(registration,indent=2)+'\n')
(root/'control-unmeasurable.json').write_text((w.PREPARED/'medium/control-preparation-failure.json').read_text())
expected={'label_scan_filter':12500,'aggregation_count_by_group':8,'topk_property_sort':100,'point_node_fetch_by_id':1,'two_hop_expand':25}
completed=[]
def validate(events,workload,iterations):
 rows=[e for e in events if e.get('event')=='result'];assert sorted(e['iteration'] for e in rows)==list(range(iterations))
 for event in rows:
  assert event['value']==expected[workload],event
  errors=verify_groups(users,event.get('rows')) if workload=='aggregation_count_by_group' else (verify_topk(users,event.get('rows')) if workload=='topk_property_sort' else [])
  assert not errors,errors
for workload in expected:
 profile='retrieval' if workload=='point_node_fetch_by_id' else 'scan'
 for mode in ['cpu','heap','counters']:
  folder=root/f'current-{workload}'/mode;folder.mkdir(parents=True)
  database=folder/'private.zyx';binary=w.CURRENT/'zyx-compare-bench'
  iterations=(100 if workload in ['point_node_fetch_by_id','two_hop_expand'] else 3) if mode=='cpu' else (3 if mode=='heap' else 1)
  command=[str(binary),'--dataset',str(data),'--prepared-db',str(w.PREPARED/'medium/current.zyx'),'--db-path',str(database),'--scale','medium','--profile',profile,'--workload',workload,'--warmup','3' if mode!='counters' else '0','--iterations',str(iterations),'--threads','1','--load-batch-rows','1000','--emit-result-rows']
  if mode=='counters':command+=['--emit-counters']
  else:command=['/usr/bin/gprofng','collect','app','-y','SIGUSR2','-H','on' if mode=='heap' else 'off','-i','off','-p','hi','-a','off','-o',str(folder/'samples.er'),'/usr/bin/stdbuf','-oL',*command]
  (folder/'command.json').write_text(json.dumps(command,indent=2)+'\n');events=[];collection={};active=False;timer=None;lock=threading.Lock();io=[]
  with (folder/'stderr.txt').open('w') as err,(folder/'raw.jsonl').open('w') as raw:
   process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=err,text=True,start_new_session=True)
   deadline=threading.Timer(1800,lambda:os.killpg(process.pid,signal.SIGKILL));deadline.daemon=True;deadline.start()
   def pause(reason):
    global active
    with lock:
     if not active:return
     active=False;delivered=True
     try:os.kill(collection['pid'],signal.SIGUSR2)
     except ProcessLookupError:delivered=False
     collection.update(pause_reason=reason,pause_monotonic_ns=time.monotonic_ns(),pause_delivered=delivered,cleanup_may_be_included=not delivered)
   try:
    for line in process.stdout:
     raw.write(line);raw.flush()
     try:event=json.loads(line)
     except ValueError:continue
     events.append(event)
     if event.get('event')!='result':continue
     if mode!='counters' and 'pid' not in collection:
      targets=[]
      for item in Path('/proc').iterdir():
       if item.name.isdigit():
        try:
         if os.readlink(item/'exe')==str(binary):targets.append(int(item.name))
        except OSError:pass
      assert len(targets)==1,targets
      collection={'pid':targets[0],'resume_after_event':event,'resume_monotonic_ns':time.monotonic_ns()};active=True
      os.kill(targets[0],signal.SIGUSR2)
      if mode=='heap':timer=threading.Timer(.2,lambda:pause('bounded_heap_window'));timer.daemon=True;timer.start()
     elif mode!='counters' and event.get('iteration')==iterations-1:pause('final_result')
     pid=collection.get('pid')
     if pid:
      try:io.append({'iteration':event['iteration'],'counters':{k:int(v) for k,v in (line.split(':',1) for line in Path(f'/proc/{pid}/io').read_text().splitlines())}})
      except (OSError,ValueError):pass
    code=process.wait(timeout=1800)
   finally:
    deadline.cancel()
    if timer:timer.cancel()
    if active:pause('process_end')
    if process.poll() is None:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
  (folder/'collection.json').write_text(json.dumps(collection,indent=2)+'\n')
  (folder/'process-io.json').write_text(json.dumps({'scope':'Linux process I/O snapshots at result boundaries; not physical database I/O. Empty means unavailable.','observations':io},indent=2)+'\n')
  assert code==0,(folder,(folder/'stderr.txt').read_text()[-1000:]);validate(events,workload,iterations)
  if mode!='counters':
   assert 'pid' in collection and 'pause_monotonic_ns' in collection,collection
   commands=[('functions','cpu-functions')] if mode=='cpu' else [('heapstat','heap-statistics'),('allocs','allocation-callstacks')]
   for flag,name in commands:
    report=subprocess.run(['/usr/bin/gprofng','display','text','-'+flag,str(folder/'samples.er')],capture_output=True,text=True,timeout=120)
    (folder/(name+'.txt')).write_text(report.stdout+report.stderr);assert report.returncode==0,(folder,name)
  for p in folder.glob('private.zyx*'):
   if p.is_file():p.unlink()
  entry={'workload':workload,'mode':mode,'validated_results':iterations,'path':str(folder),'formal_timing_samples_accepted':0};completed.append(entry);(folder/'completed.json').write_text(json.dumps(entry,indent=2)+'\n');print(json.dumps(entry),flush=True)
(root/'report.json').write_text(json.dumps({'status':'completed_separate_cpu_bounded_heap_and_counters','cases':completed,'registration':registration,'performance_gain_claim':False},indent=2)+'\n')
