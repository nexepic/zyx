from pathlib import Path
import importlib.util,subprocess,json,csv,time,resource,os,signal,re
s=importlib.util.spec_from_file_location('w','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(s);s.loader.exec_module(w);w.restrict()
while not (w.OUT/'attribution/report.json').exists():time.sleep(1)
from runner.semantic_oracle import verify_groups,verify_topk
root=w.OUT/'attribution/full-process-cpu';root.mkdir(exist_ok=True)
policy={'scope':'Whole private native process, including prepared-file cloning/open, warmups, measured repetitions and close. No import. No delayed initialization or signal filtering; sample all threads from process startup. Not query-only CPU and never formal latency.','reason':'Delayed-initialization CPU reports contain only about 0.06s CPU for a multi-second active window; do not treat those fractions as complete-query evidence. Compare captured CPU with isolated child user/system usage.','formal_timing_samples_accepted':0,'budgets_or_deadlines_changed':False}
policy['quality_limit']='Observed sampler CPU total is about 1% of isolated OS CPU usage. Treat function positions as low-coverage qualitative samples only; never infer absolute CPU time, precise shares or whole-query coverage. OS user/system usage remains separately authoritative.'
if (root/'registration.json').exists() and not (root/'registration-before-quality-limit.json').exists():
 (root/'registration-before-quality-limit.json').write_text((root/'registration.json').read_text())
(root/'registration.json').write_text(json.dumps(policy,indent=2)+'\n')
data=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation/dataset')
with (data/'users.csv').open() as stream:users=list(csv.DictReader(stream))
expected={'label_scan_filter':12500,'aggregation_count_by_group':8,'topk_property_sort':100,'point_node_fetch_by_id':1,'two_hop_expand':25};reports=[]
for workload,value in expected.items():
 folder=root/workload
 if (folder/'proof.json').exists():
  reports.append(json.loads((folder/'proof.json').read_text()));continue
 folder.mkdir();profile='retrieval' if workload=='point_node_fetch_by_id' else 'scan';iterations=100 if profile=='retrieval' or workload=='two_hop_expand' else 3
 command=['/usr/bin/gprofng','collect','app','-H','off','-i','off','-p','hi','-a','off','-o',str(folder/'samples.er'),str(w.CURRENT/'zyx-compare-bench'),'--dataset',str(data),'--prepared-db',str(w.PREPARED/'medium/current.zyx'),'--db-path',str(folder/'private.zyx'),'--scale','medium','--profile',profile,'--workload',workload,'--warmup','3','--iterations',str(iterations),'--threads','1','--load-batch-rows','1000','--emit-result-rows']
 (folder/'command.json').write_text(json.dumps(command,indent=2)+'\n');before=resource.getrusage(resource.RUSAGE_CHILDREN);start=time.monotonic()
 with (folder/'raw.jsonl').open('w') as out,(folder/'stderr.txt').open('w') as err:
  process=subprocess.Popen(command,stdout=out,stderr=err,start_new_session=True)
  try:code=process.wait(timeout=1800)
  except BaseException:
   if process.poll() is None:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
   raise
 after=resource.getrusage(resource.RUSAGE_CHILDREN);assert code==0,(folder,(folder/'stderr.txt').read_text()[-1000:])
 events=[]
 for line in (folder/'raw.jsonl').read_text().splitlines():
  try:event=json.loads(line)
  except ValueError:continue
  if event.get('event')=='result':events.append(event)
 assert len(events)==iterations
 for event in events:
  assert event['value']==value
  errors=verify_groups(users,event.get('rows')) if workload=='aggregation_count_by_group' else (verify_topk(users,event.get('rows')) if workload=='topk_property_sort' else [])
  assert not errors,errors
 result=subprocess.run(['/usr/bin/gprofng','display','text','-functions',str(folder/'samples.er')],capture_output=True,text=True,timeout=120);assert result.returncode==0
 (folder/'cpu-functions.txt').write_text(result.stdout+result.stderr)
 total=next(line for line in result.stdout.splitlines() if '<Total>' in line);sampled=float(total.split()[0]);user=after.ru_utime-before.ru_utime;system=after.ru_stime-before.ru_stime
 entry={'workload':workload,'sampled_cpu_seconds':sampled,'child_user_seconds':user,'child_system_seconds':system,'wall_seconds':time.monotonic()-start,'sampled_over_child_total':sampled/(user+system),'validated_results':len(events),'scope':policy['scope']}
 (folder/'proof.json').write_text(json.dumps(entry,indent=2)+'\n');reports.append(entry);print(json.dumps(entry),flush=True)
 for p in folder.glob('private.zyx*'):
  if p.is_file():p.unlink()
 # Preserve the low-coverage result as a limitation, never a passed CPU-coverage gate.
(root/'report.json').write_text(json.dumps({'status':'completed_low_coverage_observations_only','cases':reports,'policy':policy,'full_cpu_coverage_passed':False},indent=2)+'\n')
