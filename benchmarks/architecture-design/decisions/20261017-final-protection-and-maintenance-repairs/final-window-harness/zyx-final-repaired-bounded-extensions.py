from pathlib import Path
import json,subprocess,os,signal,sys,importlib,hashlib
sys.path.insert(0,'/tmp');owned=importlib.import_module('zyx-final-five-owned-case')
r=Path('/tmp/zyx-final-five-repaired-window-20261017')
assert (r/'read-campaign-completed.json').exists()
subprocess.run([sys.executable,'/tmp/report-zyx-final-repaired-reads.py'],check=True)
report=json.loads((r/'read-comparison-progress.json').read_text());selected={};reasons=[]
primary={'label_scan_filter','all_nodes_property_filter','label_multi_property_filter','aggregation_count_by_group','aggregation_group_by','topk_property_sort'}
for row in report['comparisons']:
 if min(row['control_processes'],row['current_processes'])!=3:continue
 c=row['control_median_process_p50_ms'];d=row['current_median_process_p50_ms'];variation=(row['control_process_p50_max_ms']-row['control_process_p50_min_ms'])/c
 near=abs((1-d/c)-.10)<=.05 if row['workload'] in primary else abs((d/c-1)-.05)<=.05
 if variation>.05 or near:
  matches=[x for x in report['processes'] if x['scale']==row['scale'] and x['workload']==row['workload'] and x['engine']=='control']
  profile=matches[0]['profile'];key=(row['scale'],profile);selected.setdefault(key,[]).append(row['workload']);reasons.append({'scale':row['scale'],'profile':profile,'workload':row['workload'],'control_p50_range_fraction':variation,'near_gate':near})
plan={'status':'registered_before_any_supplement','rule':'Original contract: uncertain range>5% or within5percentage points of gate; at most2 independent100sample processes per source/workload. No Kuzu or untriggered query reruns.','frozen_source':'63f65f37','cases':reasons,'rounds':[4,5],'samples':100,'script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'case_script_sha256':hashlib.sha256(Path('/tmp/zyx-final-repaired-extended-read.py').read_bytes()).hexdigest()}
(r/'bounded-read-extension-registration.json').write_text(json.dumps(plan,indent=2)+'\n');print(json.dumps({'extension_plan':plan}),flush=True);results=[]
for (scale,profile),workloads in selected.items():
 for number,order in [(4,['control','current']),(5,['current','control'])]:
  for engine in order:
   key=f'extended-{scale}-{profile}-{number}-{engine}';log=r/'logs'/(key+'.log');entry={'scale':scale,'profile':profile,'round':number,'engine':engine,'workloads':workloads};prior=next((x for x in results if x['scale']==scale and x['profile']==profile and x['engine']==engine and x['status']!='completed'),None)
   if prior:entry.update(status='unmeasurable_previous_failure',source=prior)
   else:
    env=dict(os.environ,ZYX_FINAL_EXTENDED_WORKLOADS=json.dumps(workloads));print(json.dumps({'event':'bounded-extension-start',**entry}),flush=True)
    with log.open('w') as output:
     p=subprocess.Popen([sys.executable,'/tmp/zyx-final-repaired-extended-read.py','--engine',engine,'--round',str(number),'--profile',profile,'--scale',scale],env=env,stdout=output,stderr=subprocess.STDOUT,start_new_session=True);timeout=False
     try:p.wait(timeout=1800)
     except subprocess.TimeoutExpired:timeout=True;os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=5)
     except BaseException:
      if p.poll() is None:os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=5)
      raise
    owned.cleanup_read(r,scale,number,engine,profile);entry.update(status='inconclusive_timeout' if timeout else ('completed' if p.returncode==0 else 'failed'),returncode=p.returncode,log=str(log))
   target=r/'extended-read-case-status'/scale/profile/f'round-{number}'/(engine+'.json');target.parent.mkdir(parents=True,exist_ok=True);target.write_text(json.dumps(entry,indent=2)+'\n');results.append(entry);print(json.dumps(entry),flush=True)
(r/'bounded-read-extensions-completed.json').write_text(json.dumps({'cases':results,'no_additional_repeat_allowed':True},indent=2)+'\n')
