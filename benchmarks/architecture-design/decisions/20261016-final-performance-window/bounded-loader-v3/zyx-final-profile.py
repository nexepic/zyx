from pathlib import Path
import importlib.util,subprocess,os,signal,json,time,threading
s=importlib.util.spec_from_file_location('window','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(s);s.loader.exec_module(w);w.restrict()
root=w.OUT/'attribution';root.mkdir(parents=True,exist_ok=False)
data=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation/dataset')
(root/'registration.json').write_text(json.dumps({'scope':'Separate instrumented processes, excluded from formal timing. Signal-based observation starts after first measured result and attempts to pause after the final result. Import/warmups are excluded; final cleanup may be included if exit wins the pause race, recorded per case. Signal delivery is approximate, not a query barrier; heap leak totals are not a database leak verdict. Timer nesting is not additive.','cases':['label_scan_filter','aggregation_count_by_group','topk_property_sort','point_node_fetch_by_id','two_hop_expand'],'engines':['control','current'],'iterations':{'scan_aggregate_sort':3,'point_traversal':100},'iteration_policy':'Collector-only repetitions for short operations; excluded from formal end-to-end timing.','mechanism_probe':'native selected-property read; typed including conversion versus generic over identical native source; full sort versus Top-K output counts disclosed'},indent=2)+'\n')
for engine in ['control','current']:
 failure=w.PREPARED/'medium'/f'{engine}-preparation-failure.json'
 if failure.exists():
  assert engine=='control';(root/'control-unmeasurable.json').write_text(failure.read_text());continue
 binary=w.CONTROL/'zyx-compare-bench-bounded' if engine=='control' else w.CURRENT/'zyx-compare-bench'
 for workload in ['label_scan_filter','aggregation_count_by_group','topk_property_sort','point_node_fetch_by_id','two_hop_expand']:
  profile='retrieval' if workload=='point_node_fetch_by_id' else 'scan';folder=root/f'{engine}-{workload}';folder.mkdir();database=folder/'private.zyx'
  os.environ['ZYX_COMPARE_BENCH']=str(binary);os.environ['ZYX_COMPARE_PREPARED_DB']=str(w.PREPARED/'medium'/f'{engine}.zyx')
  from runner.adapters.zyx import ZyxAdapter
  adapter=ZyxAdapter('zyx',data,'medium',profile=profile,threads=1);prepared=adapter.prepared_database()
  (folder/'prepared-input.json').write_text((data.parent/'prepared-input.json').read_text())
  iterations=100 if workload in ['point_node_fetch_by_id','two_hop_expand'] else 3
  command=['/usr/bin/gprofng','collect','app','-y','SIGUSR2','-H','on','-i','off','-p','hi','-a','off','-o',str(folder/'samples.er'),'/usr/bin/stdbuf','-oL',str(binary),'--dataset',str(data),'--prepared-db',str(prepared),'--db-path',str(database),'--scale','medium','--profile',profile,'--workload',workload,'--warmup','3','--iterations',str(iterations),'--threads','1','--load-batch-rows',os.environ['ZYX_COMPARE_LOAD_BATCH_ROWS'],'--emit-profile','--emit-counters','--emit-result-rows']
  (folder/'command.json').write_text(json.dumps(command,indent=2)+'\n');resumed=False;paused_after_last=False;profiled_pid=None;process_io=[]
  with (folder/'stderr.txt').open('w') as error,(folder/'raw.jsonl').open('w') as raw:
   process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=error,text=True,start_new_session=True)
   deadline=threading.Timer(1800,lambda:os.killpg(process.pid,signal.SIGKILL));deadline.daemon=True;deadline.start()
   try:
    for line in process.stdout:
     raw.write(line);raw.flush()
     try:event=json.loads(line)
     except ValueError:continue
     if event.get('event')=='result' and profiled_pid is not None:
      try:
       counters={key:int(value) for key,value in (line.split(':',1) for line in Path(f'/proc/{profiled_pid}/io').read_text().splitlines())}
       process_io.append({'iteration':event.get('iteration'),'monotonic_ns':time.monotonic_ns(),'counters':counters})
      except (OSError,ValueError):pass
     if event.get('event')=='result' and not resumed:
      targets=[]
      for item in Path('/proc').iterdir():
       if item.name.isdigit():
        try:
         if (item/'comm').read_text().strip()=='zyx-compare-ben':targets.append(int(item.name))
        except (OSError,PermissionError):pass
      # Linux comm is truncated to 15 chars; only one measured process is active.
      if not targets:
       for item in Path('/proc').iterdir():
        if item.name.isdigit():
         try:
          if os.readlink(item/'exe')==str(binary):targets.append(int(item.name))
         except OSError:pass
      if len(targets)!=1:raise RuntimeError(f'cannot identify single profiled child: {targets}')
      profiled_pid=targets[0]
      try:process_io.append({'iteration':event.get('iteration'),'monotonic_ns':time.monotonic_ns(),'counters':{key:int(value) for key,value in (line.split(':',1) for line in Path(f'/proc/{profiled_pid}/io').read_text().splitlines())}})
      except (OSError,ValueError):pass
      os.kill(profiled_pid,signal.SIGUSR2);resumed=True;(folder/'resume.json').write_text(json.dumps({'pid':profiled_pid,'after_event':event})+'\n')
     elif event.get('event')=='result' and resumed and event.get('iteration')==iterations-1 and not paused_after_last:
      pause_delivered=True
      try:os.kill(profiled_pid,signal.SIGUSR2)
      except ProcessLookupError:pause_delivered=False
      paused_after_last=True;(folder/'pause.json').write_text(json.dumps({'pid':profiled_pid,'after_event':event,'pause_signal_delivered':pause_delivered,'cleanup_may_be_included':not pause_delivered})+'\n')
    status=process.wait(timeout=1800)
   finally:
    deadline.cancel()
    if process.poll() is None:
     try:os.killpg(process.pid,signal.SIGKILL)
     except ProcessLookupError:pass
     process.wait(timeout=5)
  (folder/'process-io.json').write_text(json.dumps({'scope':'Process-level Linux I/O snapshots at result boundaries; includes collector/protocol writes and is not physical database I/O. Native page/decode/write counters are separately retained in raw profile events. gprofng I/O collector is unavailable on this host (llseek initialization error), so it is disabled rather than reporting false zeros.','observations':process_io},indent=2)+'\n')
  if status or not resumed or not paused_after_last:raise RuntimeError(f'profile case failed: {folder}, status={status}')
  for flag,name in [('functions','cpu-functions'),('heapstat','heap-statistics'),('allocs','allocation-callstacks')]:
   report=subprocess.run(['/usr/bin/gprofng','display','text','-'+flag,str(folder/'samples.er')],capture_output=True,text=True,timeout=120)
   (folder/(name+'.txt')).write_text(report.stdout+report.stderr)
   if report.returncode:raise RuntimeError(f'collector report failed: {folder}, {flag}')
  for private in folder.glob('private.zyx*'):
   if private.is_file():private.unlink()
  print(json.dumps({'profile_completed':engine+'-'+workload,'status':status}),flush=True)
