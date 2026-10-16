from pathlib import Path
import importlib.util,subprocess,os,signal,json,time
s=importlib.util.spec_from_file_location('window','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(s);s.loader.exec_module(w);w.restrict()
root=w.OUT/'attribution';root.mkdir(parents=True,exist_ok=False)
data=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation/dataset')
(root/'registration.json').write_text(json.dumps({'scope':'Separate instrumented processes; excluded from formal timing. Resume CPU/heap/I/O collector after first measured result, excluding import and warmups. Timer nesting is not additive.','cases':['label_scan_filter','aggregation_count_by_group','topk_property_sort','point_node_fetch_by_id','two_hop_expand'],'engines':['control','current'],'iterations':10,'mechanism_probe':'native selected-property read; typed including conversion versus generic over identical native source; full sort versus Top-K output counts disclosed'},indent=2)+'\n')
for engine in ['control','current']:
 binary=w.CONTROL/'zyx-compare-bench-bounded' if engine=='control' else w.CURRENT/'zyx-compare-bench'
 for workload in ['label_scan_filter','aggregation_count_by_group','topk_property_sort','point_node_fetch_by_id','two_hop_expand']:
  profile='retrieval' if workload=='point_node_fetch_by_id' else 'scan';folder=root/f'{engine}-{workload}';folder.mkdir();database=folder/'private.zyx'
  os.environ['ZYX_COMPARE_BENCH']=str(binary);os.environ['ZYX_COMPARE_PREPARED_DB']=str(w.PREPARED/'medium'/f'{engine}.zyx')
  from runner.adapters.zyx import ZyxAdapter
  adapter=ZyxAdapter('zyx',data,'medium',profile=profile,threads=1);prepared=adapter.prepared_database()
  (folder/'prepared-input.json').write_text((data.parent/'prepared-input.json').read_text())
  command=['/usr/bin/gprofng','collect','app','-y','SIGUSR2','-H','on','-i','on','-p','hi','-a','off','-o',str(folder/'samples.er'),'/usr/bin/stdbuf','-oL',str(binary),'--dataset',str(data),'--prepared-db',str(prepared),'--db-path',str(database),'--scale','medium','--profile',profile,'--workload',workload,'--warmup','3','--iterations','10','--threads','1','--load-batch-rows',os.environ['ZYX_COMPARE_LOAD_BATCH_ROWS'],'--emit-profile','--emit-counters','--emit-result-rows']
  (folder/'command.json').write_text(json.dumps(command,indent=2)+'\n');resumed=False
  with (folder/'stderr.txt').open('w') as error,(folder/'raw.jsonl').open('w') as raw:
   process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=error,text=True)
   for line in process.stdout:
    raw.write(line);raw.flush()
    try:event=json.loads(line)
    except ValueError:continue
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
     os.kill(targets[0],signal.SIGUSR2);resumed=True;(folder/'resume.json').write_text(json.dumps({'pid':targets[0],'after_event':event})+'\n')
   status=process.wait(timeout=1800)
  if status or not resumed:raise RuntimeError(f'profile case failed: {folder}, status={status}')
  report=subprocess.run(['/usr/bin/gprofng','display','text','-functions',str(folder/'samples.er')],capture_output=True,text=True,timeout=120)
  (folder/'cpu-functions.txt').write_text(report.stdout+report.stderr)
  for private in folder.glob('private.zyx*'):
   if private.is_file():private.unlink()
  print(json.dumps({'profile_completed':engine+'-'+workload,'status':status}),flush=True)
